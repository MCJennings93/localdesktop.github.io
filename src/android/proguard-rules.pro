-keep class io.github.localdesktop.KeyboardShortcutsPrompt {
    public static void showIfNeeded(android.app.Activity);
}
-keep class io.github.localdesktop.SetupPage {
    public static void show(android.app.Activity, java.lang.String);
}
-keep class io.github.localdesktop.SessionService {
    public static void start(android.app.Activity);
    public static void sessionEnded(android.app.Activity, boolean, boolean);
}
