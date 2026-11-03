package io.github.localdesktop;

import android.app.Notification;
import android.app.PendingIntent;
import android.os.Bundle;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

/**
 * Shows Android's notifications on the Linux desktop, and cancels them when they're dismissed there.
 * It runs only if the user turns it on under Android's notification access. See {@link NotificationBridge}.
 */
public final class NotificationBridgeService extends NotificationListenerService {
    private static volatile NotificationBridgeService instance;

    @Override
    public void onListenerConnected() {
        instance = this;
        sendActive();
    }

    @Override
    public void onListenerDisconnected() {
        instance = null;
    }

    @Override
    public void onNotificationPosted(StatusBarNotification notification) {
        send(notification);
    }

    @Override
    public void onNotificationRemoved(StatusBarNotification notification) {
        if (!mine(notification)) {
            NotificationBridge.androidRemoved(notification.getKey());
        }
    }

    private boolean mine(StatusBarNotification notification) {
        return getPackageName().equals(notification.getPackageName());
    }

    /**
     * Not this app's own, which would feed the two directions into each other, nor what the user can't
     * dismiss (media, running services) or isn't a message (group summaries, hidden content).
     */
    private void send(StatusBarNotification notification) {
        Notification content = notification.getNotification();
        if (mine(notification)
                || !notification.isClearable()
                || (content.flags & Notification.FLAG_GROUP_SUMMARY) != 0
                || content.visibility == Notification.VISIBILITY_SECRET) {
            return;
        }
        Bundle extras = content.extras;
        String title = text(extras, Notification.EXTRA_TITLE);
        String body = text(extras, Notification.EXTRA_BIG_TEXT);
        if (body.isEmpty()) {
            body = text(extras, Notification.EXTRA_TEXT);
        }
        if (title.isEmpty() && body.isEmpty()) {
            return;
        }
        String app = NotificationBridge.appLabel(notification.getPackageName());
        NotificationBridge.androidPosted(notification.getKey(), app, title, body);
    }

    private static String text(Bundle extras, String name) {
        CharSequence value = extras.getCharSequence(name);
        return value == null ? "" : value.toString();
    }

    /** The bridge just connected, or this service did: tell the desktop what's already here. */
    static void sendActive() {
        NotificationBridgeService service = instance;
        if (service == null) {
            return;
        }
        try {
            StatusBarNotification[] active = service.getActiveNotifications();
            if (active != null) {
                for (StatusBarNotification notification : active) {
                    service.send(notification);
                }
            }
        } catch (RuntimeException e) {
            android.util.Log.w("NotificationBridge", "Can't list notifications: " + e);
        }
    }

    /** The user dismissed this one on the desktop. */
    static void cancel(String key) {
        NotificationBridgeService service = instance;
        if (service != null) {
            service.cancelNotification(key);
        }
    }

    /** The user clicked this one on the desktop. */
    static void open(String key) {
        NotificationBridgeService service = instance;
        if (service == null) {
            return;
        }
        StatusBarNotification[] found = service.getActiveNotifications(new String[] {key});
        if (found == null || found.length == 0) {
            return;
        }
        PendingIntent intent = found[0].getNotification().contentIntent;
        if (intent != null) {
            try {
                intent.send();
            } catch (PendingIntent.CanceledException e) {
                android.util.Log.w("NotificationBridge", "Its app no longer takes this: " + e);
            }
        }
    }
}
