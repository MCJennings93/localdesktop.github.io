package io.github.localdesktop;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.service.notification.StatusBarNotification;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The Android end of the notification bridge: it connects to the guest agent
 * (/usr/local/bin/localdesktop-notify-bridge, which describes the messages) and mirrors notifications
 * both ways, dismissals included. Linux notifications show here on their own channel; Android ones
 * come from {@link NotificationBridgeService}, if the user has granted notification access.
 *
 * Messages from the guest are untrusted: only contents cross, with their sizes limited here.
 */
final class NotificationBridge {
    /** Tags the mirrored Linux notifications, whose ids are the desktop's. */
    private static final String TAG = "linux";
    private static final String CHANNEL = "linux";
    static final String ACTION_DISMISSED = "io.github.localdesktop.NOTIFICATION_DISMISSED";
    static final String EXTRA_ID = "id";
    private static final int MAX_LINE = 64 * 1024;
    private static final int MAX_TITLE = 512;
    private static final int MAX_BODY = 4096;
    private static final long RETRY_MS = 2000;

    private static Context context;
    private static boolean started;
    /** Writes never block the listener service's callbacks. */
    private static final ExecutorService writer = Executors.newSingleThreadExecutor();
    private static volatile OutputStream output;

    private NotificationBridge() {}

    static synchronized void start(Context anyContext) {
        if (started) {
            return;
        }
        started = true;
        context = anyContext.getApplicationContext();
        cancelMirrors();
        if (Build.VERSION.SDK_INT >= 26) {
            context.getSystemService(NotificationManager.class)
                    .createNotificationChannel(
                            new NotificationChannel(CHANNEL, "Linux desktop", NotificationManager.IMPORTANCE_DEFAULT));
        }
        final String path = context.getFilesDir() + "/arch/tmp/localdesktop-notify.sock";
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                while (true) {
                    connectAndRead(path);
                    try {
                        Thread.sleep(RETRY_MS);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }, "notification-bridge");
        thread.setDaemon(true);
        thread.start();
    }

    /** Runs while the agent is there, which is while the session is. */
    private static void connectAndRead(String path) {
        try (LocalSocket socket = new LocalSocket()) {
            socket.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
            output = socket.getOutputStream();
            sendActive();
            InputStream input = socket.getInputStream();
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            boolean overlong = false;
            int count;
            while ((count = input.read(buffer)) > 0) {
                for (int i = 0; i < count; i++) {
                    if (buffer[i] == '\n') {
                        if (!overlong) {
                            handle(new String(line.toByteArray(), StandardCharsets.UTF_8));
                        }
                        line.reset();
                        overlong = false;
                    } else if (line.size() >= MAX_LINE) {
                        overlong = true;
                    } else {
                        line.write(buffer[i]);
                    }
                }
            }
        } catch (IOException e) {
            // No agent yet, or it's gone with the session.
        } finally {
            if (output != null) {
                output = null;
                cancelMirrors();
            }
        }
    }

    private static void handle(String line) {
        try {
            JSONObject message = new JSONObject(line);
            switch (message.getString("type")) {
                case "post":
                    show(
                            message.getInt("id"),
                            message.optString("app"),
                            message.optString("title"),
                            message.optString("body"));
                    break;
                case "close":
                    manager().cancel(TAG, message.getInt("id"));
                    break;
                case "cancel":
                    NotificationBridgeService.cancel(message.getString("key"));
                    break;
                case "open":
                    NotificationBridgeService.open(message.getString("key"));
                    break;
                default:
                    break;
            }
        } catch (JSONException | RuntimeException e) {
            android.util.Log.w("NotificationBridge", "Ignoring a bad message: " + e);
        }
    }

    private static void show(int id, String app, String title, String body) {
        Intent open = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        Intent dismissed = new Intent(context, SessionService.class).setAction(ACTION_DISMISSED).putExtra(EXTRA_ID, id);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(context, CHANNEL)
                : new Notification.Builder(context);
        builder.setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle(limit(title, MAX_TITLE))
                .setContentText(limit(body, MAX_BODY))
                .setStyle(new Notification.BigTextStyle().bigText(limit(body, MAX_BODY)))
                .setSubText(limit(app, MAX_TITLE))
                .setAutoCancel(true)
                .setContentIntent(PendingIntent.getActivity(context, 0, open, PendingIntent.FLAG_IMMUTABLE))
                // Android sends this only when the user dismisses it. The id tells them apart.
                .setDeleteIntent(PendingIntent.getService(
                        context, id, dismissed, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        manager().notify(TAG, id, builder.build());
    }

    private static String limit(String text, int length) {
        return text.length() > length ? text.substring(0, length) : text;
    }

    private static NotificationManager manager() {
        return context.getSystemService(NotificationManager.class);
    }

    /** With the session gone, or after the process was, nothing can answer for these. */
    static void cancelMirrors() {
        if (context == null || Build.VERSION.SDK_INT < 23) {
            return;
        }
        for (StatusBarNotification active : manager().getActiveNotifications()) {
            if (TAG.equals(active.getTag())) {
                manager().cancel(TAG, active.getId());
            }
        }
    }

    /** The user dismissed a mirrored Linux notification. */
    static void dismissed(int id) {
        try {
            send(new JSONObject().put("type", "dismissed").put("id", id));
        } catch (JSONException e) {
            // Only numbers and a fixed name go in.
        }
    }

    static void androidPosted(String key, String app, String title, String body) {
        try {
            send(new JSONObject()
                    .put("type", "post")
                    .put("key", key)
                    .put("app", limit(app, MAX_TITLE))
                    .put("title", limit(title, MAX_TITLE))
                    .put("body", limit(body, MAX_BODY)));
        } catch (JSONException e) {
            android.util.Log.w("NotificationBridge", "Can't send a notification: " + e);
        }
    }

    static void androidRemoved(String key) {
        try {
            send(new JSONObject().put("type", "remove").put("key", key));
        } catch (JSONException e) {
            android.util.Log.w("NotificationBridge", "Can't send a removal: " + e);
        }
    }

    /** Dropped if the session isn't up: what's on Android is sent again when it is. */
    private static void send(JSONObject message) {
        final OutputStream target = output;
        if (target == null) {
            return;
        }
        final byte[] bytes = (message + "\n").getBytes(StandardCharsets.UTF_8);
        writer.execute(new Runnable() {
            @Override
            public void run() {
                try {
                    target.write(bytes);
                    target.flush();
                } catch (IOException e) {
                    // The reader notices too.
                }
            }
        });
    }

    private static void sendActive() {
        NotificationBridgeService.sendActive();
    }

    /** What the desktop calls an app: the label Android shows for it. */
    static String appLabel(String packageName) {
        Context anyContext = context;
        if (anyContext == null) {
            return packageName;
        }
        PackageManager packages = anyContext.getPackageManager();
        try {
            return String.valueOf(packages.getApplicationLabel(packages.getApplicationInfo(packageName, 0)));
        } catch (PackageManager.NameNotFoundException e) {
            return packageName;
        }
    }
}
