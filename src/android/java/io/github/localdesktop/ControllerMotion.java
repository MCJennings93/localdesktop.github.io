package io.github.localdesktop;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.InputDevice;

/**
 * Follows the game controllers paired with Android for the native side: each one connecting and going
 * away (src/android/app/controller_slots.rs gives some a slot, and so a controller in Steam, of their
 * own), and the gyro and accelerometer of one (src/android/app/controller_motion.rs).
 *
 * Android 12 and later give each controller whose kernel driver exposes its motion sensors
 * (DualShock 4, DualSense, and Switch controllers on Linux 5.16+) its own {@link SensorManager}. The
 * sensors are read only while Steam wants motion from that controller, checked twice a second, and
 * from the controller connected last that has them.
 */
final class ControllerMotion implements InputManager.InputDeviceListener, SensorEventListener {
    private static final String TAG = "ControllerMotion";
    private static final int ACCELEROMETER = 0;
    private static final int GYROSCOPE = 1;
    private static final int SAMPLING_US = 4000;
    private static final long POLL_MS = 500;

    private static ControllerMotion instance;

    private final InputManager input;
    private final Handler handler;
    /** The controller whose sensors are used, or -1. */
    private int device = -1;
    private SensorManager sensors;
    private boolean reading;
    private int idlePolls;
    private final Runnable poller = new Runnable() {
        @Override
        public void run() {
            poll();
        }
    };

    private static native void nativeControllerAdded(int device, int vendor);

    private static native void nativeControllerRemoved(int device);

    private static native void nativeControllersRefresh();

    private static native boolean nativeMotionWanted(int device);

    private static native void nativeMotion(int device, int kind, float x, float y, float z);

    private static native void nativeMotionStopped(int device);

    static synchronized void start(Context context) {
        if (instance != null) {
            return;
        }
        HandlerThread thread = new HandlerThread("controller-motion", android.os.Process.THREAD_PRIORITY_DISPLAY);
        thread.start();
        instance = new ControllerMotion(context.getApplicationContext(), new Handler(thread.getLooper()));
    }

    private ControllerMotion(Context context, Handler handler) {
        this.handler = handler;
        input = context.getSystemService(InputManager.class);
        final ControllerMotion self = this;
        handler.post(new Runnable() {
            @Override
            public void run() {
                input.registerInputDeviceListener(self, self.handler);
                for (int id : input.getInputDeviceIds()) {
                    onInputDeviceAdded(id);
                }
                poll();
            }
        });
    }

    /**
     * A gamepad or joystick. The source flags share class bits (SOURCE_GAMEPAD includes the button
     * class every keyboard has, so a touchpad with keys would match a plain mask): require the whole
     * flag.
     */
    private static boolean isController(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** A controller's own gyro and accelerometer, or null. */
    private static SensorManager motionSensors(InputDevice device) {
        if (Build.VERSION.SDK_INT < 31 || !isController(device)) {
            return null;
        }
        SensorManager manager = device.getSensorManager();
        if (manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE) == null
                && manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) == null) {
            return null;
        }
        return manager;
    }

    private void poll() {
        // A new session makes new rings: put the controllers' slots back into them.
        nativeControllersRefresh();
        boolean wanted = device >= 0 && nativeMotionWanted(device);
        idlePolls = wanted ? 0 : idlePolls + 1;
        if (wanted && !reading) {
            listen();
        } else if (reading && idlePolls >= 2) {
            stopListening();
            Log.i(TAG, "paused: Steam wants no motion");
        }
        handler.postDelayed(poller, POLL_MS);
    }

    private void listen() {
        Sensor gyroscope = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        Sensor accelerometer = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (gyroscope != null) {
            sensors.registerListener(this, gyroscope, SAMPLING_US, handler);
        }
        if (accelerometer != null) {
            sensors.registerListener(this, accelerometer, SAMPLING_US, handler);
        }
        reading = true;
        Log.i(TAG, "reading controller " + device + ": gyro " + (gyroscope != null ? gyroscope.getName() : "none")
                + ", accelerometer " + (accelerometer != null ? accelerometer.getName() : "none"));
    }

    private void stopListening() {
        if (reading) {
            sensors.unregisterListener(this);
            reading = false;
            nativeMotionStopped(device);
        }
    }

    @Override
    public void onInputDeviceAdded(int id) {
        added(id, true);
    }

    /** A controller (or one that changed): its slot, and its sensors if {@code adopt} or none are read. */
    private void added(int id, boolean adopt) {
        InputDevice added = input.getInputDevice(id);
        if (!isController(added)) {
            return;
        }
        nativeControllerAdded(id, added.getVendorId());
        SensorManager manager = motionSensors(added);
        if (manager == null || (!adopt && device >= 0) || id == device) {
            return;
        }
        stopListening();
        device = id;
        sensors = manager;
        Log.i(TAG, "controller " + id + " (" + added.getName() + ") has motion sensors");
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        nativeControllerRemoved(id);
        if (id != device) {
            return;
        }
        stopListening();
        device = -1;
        sensors = null;
        // Fall back to another connected controller with motion sensors.
        for (int other : input.getInputDeviceIds()) {
            if (device < 0) {
                InputDevice candidate = input.getInputDevice(other);
                SensorManager manager = motionSensors(candidate);
                if (manager != null) {
                    device = other;
                    sensors = manager;
                }
            }
        }
    }

    @Override
    public void onInputDeviceChanged(int id) {
        if (id == device && motionSensors(input.getInputDevice(id)) == null) {
            onInputDeviceRemoved(id);
        } else {
            added(id, false);
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        int kind;
        switch (event.sensor.getType()) {
            case Sensor.TYPE_GYROSCOPE:
                kind = GYROSCOPE;
                break;
            case Sensor.TYPE_ACCELEROMETER:
                kind = ACCELEROMETER;
                break;
            default:
                return;
        }
        nativeMotion(device, kind, event.values[0], event.values[1], event.values[2]);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}
}
