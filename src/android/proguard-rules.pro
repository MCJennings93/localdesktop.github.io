-keep class io.github.localdesktop.KeyboardShortcutsPrompt {
    public static void showIfNeeded(android.app.Activity);
}
-keep class io.github.localdesktop.SetupPage {
    public static void show(android.app.Activity, java.lang.String);
}
-keep class io.github.localdesktop.SessionService {
    public static void start(android.app.Activity);
    public static void sessionEnded(android.app.Activity, boolean, boolean);
    private static native void nativeDnsServersChanged(java.lang.String[]);
}
-keep class io.github.localdesktop.ControllerMotion {
    private static native void nativeControllerAdded(int, int);
    private static native void nativeControllerRemoved(int);
    private static native void nativeControllersRefresh();
    private static native boolean nativeMotionWanted(int);
    private static native void nativeMotion(int, int, float, float, float);
    private static native void nativeMotionStopped(int);
}

-keep class io.github.localdesktop.ExternalDisplay {
    public static void start(android.app.Activity);
    public static void background(android.app.Activity);
    public static void showKeyboard(android.app.Activity);
    public static void stop(android.app.Activity);
    native <methods>;
}

-keep class io.github.localdesktop.TrackpadControls {
    native <methods>;
}

-keep class io.github.localdesktop.PhoneKeyboard {
    native <methods>;
}
