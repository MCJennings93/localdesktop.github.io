package io.github.localdesktop;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.provider.Settings;

/** Asks once whether Android's notifications should show on the Linux desktop too. */
final class NotificationAccessPrompt {
    private static final String PREFS = "notification_access";
    private static final String ANSWERED = "answered";

    private NotificationAccessPrompt() {}

    static void showIfNeeded(final Activity activity) {
        final SharedPreferences prefs = activity.getSharedPreferences(PREFS, Activity.MODE_PRIVATE);
        if (prefs.getBoolean(ANSWERED, false) || isEnabled(activity)) {
            return;
        }
        CharSequence app = activity.getApplicationInfo().loadLabel(activity.getPackageManager());
        new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Show Android notifications on the desktop?")
                .setMessage(
                        "Notifications from the Linux desktop already show on Android. "
                                + app + " can show Android's on the desktop too, and dismissing one on either side "
                                + "dismisses it on both.\n\n"
                                + "This needs Android's notification access. With it on, " + app + " reads the text "
                                + "of your notifications and passes it to the desktop on this phone, where its "
                                + "programs could read it too. It doesn't send them anywhere else.\n\n"
                                + "You can turn it off at any time in Android's settings.")
                .setCancelable(false)
                .setPositiveButton("Turn on", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        prefs.edit().putBoolean(ANSWERED, true).apply();
                        activity.startActivity(new Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"));
                    }
                })
                .setNegativeButton("No thanks", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        prefs.edit().putBoolean(ANSWERED, true).apply();
                    }
                })
                .show();
    }

    private static boolean isEnabled(Activity activity) {
        String enabled = Settings.Secure.getString(activity.getContentResolver(), "enabled_notification_listeners");
        String service = new ComponentName(activity, NotificationBridgeService.class).flattenToString();
        return enabled != null && enabled.contains(service);
    }
}
