package io.github.localdesktop;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;
import android.widget.Toast;
import java.net.InetAddress;
import java.util.List;

/**
 * Tells Android the desktop is in use while the app's window is hidden, so it keeps the app and its
 * guest processes running. Closing the app (the notification's button, or swiping it from recents)
 * ends the process, and the session with it.
 */
public final class SessionService extends Service {
    static {
        System.loadLibrary("localdesktop");
    }

    private static final String CHANNEL = "session";
    private static final int NOTIFICATION = 1;
    private static final String ACTION_STOP = "io.github.localdesktop.STOP";
    private static final String NOTIFICATIONS_PERMISSION = "android.permission.POST_NOTIFICATIONS";
    // Android hides a quiet notification's buttons until it is expanded, so say where the button is.
    private static final String RUNNING = "The desktop keeps running in the background. Expand this to close it.";
    private static final String STOPPED = "The desktop stopped. Open the app to start it again.";

    private static boolean watchingNetwork;

    private static native void nativeDnsServersChanged(String[] servers);

    public static void start(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                watchNetwork(activity);
                NotificationBridge.start(activity);
                ControllerMotion.start(activity);
                NotificationAccessPrompt.showIfNeeded(activity);
                if (Build.VERSION.SDK_INT >= 33
                        && activity.checkSelfPermission(NOTIFICATIONS_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
                    activity.requestPermissions(new String[] {NOTIFICATIONS_PERMISSION}, 0);
                }
                Intent intent = new Intent(activity, SessionService.class);
                if (Build.VERSION.SDK_INT >= 26) {
                    activity.startForegroundService(intent);
                } else {
                    activity.startService(intent);
                }
            }
        });
    }

    /** Keeps the guest's DNS servers those of Android's default network, as it changes. */
    private static void watchNetwork(Activity activity) {
        if (watchingNetwork) {
            return;
        }
        watchingNetwork = true;
        ConnectivityManager manager = activity.getApplicationContext().getSystemService(ConnectivityManager.class);
        manager.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
            @Override
            public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                List<InetAddress> addresses = properties.getDnsServers();
                String[] servers = new String[addresses.size()];
                for (int i = 0; i < servers.length; i++) {
                    servers[i] = addresses.get(i).getHostAddress();
                }
                nativeDnsServersChanged(servers);
            }
        });
    }

    /** The session ended without being asked to. */
    public static void sessionEnded(final Activity activity, final boolean visible, final boolean restarting) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!restarting) {
                    NotificationManager manager = activity.getSystemService(NotificationManager.class);
                    manager.notify(NOTIFICATION, notification(activity, STOPPED));
                }
                if (visible) {
                    Toast.makeText(activity, restarting ? "The desktop stopped. Restarting it." : STOPPED, Toast.LENGTH_LONG)
                            .show();
                }
            }
        });
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            close();
            return START_NOT_STICKY;
        }
        if (intent != null && NotificationBridge.ACTION_DISMISSED.equals(intent.getAction())) {
            NotificationBridge.dismissed(intent.getIntExtra(NotificationBridge.EXTRA_ID, 0));
            return START_NOT_STICKY;
        }
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel =
                    new NotificationChannel(CHANNEL, "Desktop session", NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION, notification(this, RUNNING), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION, notification(this, RUNNING));
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        close();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void close() {
        NotificationBridge.cancelMirrors();
        stopForeground(true);
        stopSelf();
        Process.killProcess(Process.myPid());
    }

    private static Notification notification(Context context, String text) {
        Intent open = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        Intent stop = new Intent(context, SessionService.class).setAction(ACTION_STOP);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(context, CHANNEL)
                : new Notification.Builder(context);
        return builder
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle("Local Desktop")
                .setContentText(text)
                .setOngoing(true)
                .setContentIntent(PendingIntent.getActivity(context, 0, open, PendingIntent.FLAG_IMMUTABLE))
                .addAction(new Notification.Action.Builder(
                                null,
                                "Close desktop",
                                PendingIntent.getService(context, 1, stop, PendingIntent.FLAG_IMMUTABLE))
                        .build())
                .build();
    }
}
