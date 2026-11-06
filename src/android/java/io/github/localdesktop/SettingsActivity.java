package io.github.localdesktop;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.text.InputType;
import android.util.TypedValue;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Local Desktop's settings, opened from the app's page in Android's settings. It runs in its own
 * process, so a desktop that is stuck or keeps crashing can still be fixed from here.
 *
 * The desktop reads these when it starts (see `core::config` and `ApplicationContext`), and
 * writes what it starts with to the "defaults" preferences for "Restore defaults".
 */
public final class SettingsActivity extends Activity {
    static final String PREFERENCES = "settings";
    static final String DEFAULTS = "defaults";
    private static final String[] TEXTS = {"username", "check", "install", "launch"};
    /** The on/off settings, by key; their defaults are in the defaults preferences. */
    private static final String[] SWITCHES = {"android_keyboard", "native_touch"};

    private final Switch[] switches = new Switch[SWITCHES.length];
    private final EditText[] texts = new EditText[TEXTS.length];

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (getActionBar() != null) {
            // Android 15 lays the page out under it; the title goes in the page instead.
            getActionBar().hide();
        }
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(16), dp(8), dp(16), dp(24));
        TextView title = new TextView(this);
        title.setText("Local Desktop settings");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28);
        title.setPadding(0, dp(24), 0, 0);
        list.addView(title);

        heading(list, "Desktop");
        switches[0] = toggle(list, "Use the Android keyboard");
        note(list, "Your phone's keyboard comes up when a text field takes focus. Off: Squeekboard, the desktop's own on-screen keyboard.");
        switches[1] = toggle(list, "Touch as touch");
        note(list, "Touches reach the desktop as touches, for touch-first desktops such as Plasma Mobile, which drag, flick and swipe from the edges themselves. Off: a touch is a mouse pointer, for desktops such as Xfce.");
        texts[0] = field(list, "User", false);
        note(list, "Log in as this user. It has to exist in Linux first.");

        heading(list, "Commands");
        note(list, "How Local Desktop checks for, installs and starts the desktop environment.");
        note(list, "/etc/localdesktop/localdesktop.toml in Linux, if there is one, overrides what it sets of these and the switches above: an installer there (such as the Plasma Mobile one) chooses its desktop that way.");
        texts[1] = field(list, "Check", true);
        texts[2] = field(list, "Install", true);
        texts[3] = field(list, "Launch", true);

        Button restore = new Button(this);
        restore.setText("Restore defaults");
        restore.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                SharedPreferences.Editor editor = getSharedPreferences(PREFERENCES, MODE_PRIVATE).edit();
                for (String key : SWITCHES) {
                    editor.remove(key);
                }
                for (String key : TEXTS) {
                    editor.remove(key);
                }
                editor.commit();
                load();
            }
        });
        Button restart = new Button(this);
        restart.setText("Restart Local Desktop");
        restart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                save();
                restartDesktop();
            }
        });
        LinearLayout buttons = new LinearLayout(this);
        buttons.setPadding(0, dp(16), 0, 0);
        buttons.addView(restore);
        buttons.addView(restart);
        list.addView(buttons);
        note(list, "Changes apply the next time Local Desktop starts.");

        // Opens at the top, without a field taking focus and bringing up the keyboard.
        list.setFocusableInTouchMode(true);
        list.requestFocus();
        final ScrollView scroll = new ScrollView(this);
        scroll.addView(list);
        if (Build.VERSION.SDK_INT >= 30) {
            // Clear of the status and navigation bars, which Android 15 draws over the page.
            scroll.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @Override
                public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                    android.graphics.Insets bars = insets.getInsets(
                            WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
                    view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                    return insets;
                }
            });
        }
        setContentView(scroll);
        load();
    }

    @Override
    protected void onPause() {
        super.onPause();
        save();
    }

    /** What is set, and the defaults for the rest. */
    private void load() {
        SharedPreferences preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        SharedPreferences defaults = getSharedPreferences(DEFAULTS, MODE_PRIVATE);
        for (int i = 0; i < SWITCHES.length; i++) {
            switches[i].setChecked(preferences.getBoolean(SWITCHES[i], defaults.getBoolean(SWITCHES[i], false)));
        }
        for (int i = 0; i < TEXTS.length; i++) {
            texts[i].setText(preferences.getString(TEXTS[i], defaults.getString(TEXTS[i], "")));
        }
    }

    /**
     * Keeps only what differs from the defaults, so a default that changes in an update still
     * reaches settings nobody touched.
     */
    private void save() {
        SharedPreferences defaults = getSharedPreferences(DEFAULTS, MODE_PRIVATE);
        SharedPreferences.Editor editor = getSharedPreferences(PREFERENCES, MODE_PRIVATE).edit();
        for (int i = 0; i < SWITCHES.length; i++) {
            if (switches[i].isChecked() == defaults.getBoolean(SWITCHES[i], false)) {
                editor.remove(SWITCHES[i]);
            } else {
                editor.putBoolean(SWITCHES[i], switches[i].isChecked());
            }
        }
        for (int i = 0; i < TEXTS.length; i++) {
            String value = texts[i].getText().toString().trim();
            if (value.isEmpty() || value.equals(defaults.getString(TEXTS[i], null))) {
                editor.remove(TEXTS[i]);
            } else {
                editor.putString(TEXTS[i], value);
            }
        }
        // Written before the desktop restarts and reads them.
        editor.commit();
    }

    /** Ends the desktop's process, which takes its Linux processes with it, and starts it again. */
    private void restartDesktop() {
        ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        if (manager.getRunningAppProcesses() != null) {
            for (ActivityManager.RunningAppProcessInfo info : manager.getRunningAppProcesses()) {
                if (info.processName.equals(getPackageName())) {
                    Process.killProcess(info.pid);
                }
            }
        }
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch == null) {
            Toast.makeText(this, "Open Local Desktop to apply the changes.", Toast.LENGTH_LONG).show();
            return;
        }
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(launch);
        finish();
    }

    private void heading(LinearLayout list, String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setTextColor(accentColor());
        view.setPadding(0, dp(24), 0, 0);
        list.addView(view);
    }

    private void note(LinearLayout list, String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        view.setAlpha(0.7f);
        view.setPadding(0, dp(4), 0, 0);
        list.addView(view);
    }

    private Switch toggle(LinearLayout list, String label) {
        Switch toggle = new Switch(this);
        toggle.setText(label);
        toggle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        toggle.setPadding(0, dp(12), 0, 0);
        list.addView(toggle);
        return toggle;
    }

    private EditText field(LinearLayout list, String label, boolean command) {
        TextView title = new TextView(this);
        title.setText(label);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        title.setPadding(0, dp(16), 0, 0);
        list.addView(title);
        EditText edit = new EditText(this);
        // Empty until Local Desktop has started once and written its defaults; empty means those.
        edit.setHint("Default");
        edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, command ? 13 : 16);
        if (command) {
            edit.setTypeface(Typeface.MONOSPACE);
            edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                    | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        } else {
            edit.setSingleLine(true);
            edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        }
        list.addView(edit);
        return edit;
    }

    private int accentColor() {
        TypedValue value = new TypedValue();
        getTheme().resolveAttribute(android.R.attr.colorAccent, value, true);
        return value.data;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
