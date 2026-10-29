use jni::objects::{JClass, JObject};
use jni::sys::jobject;
use jni::JNIEnv;
use winit::platform::android::activity::AndroidApp;

/// A function that can be passed into `run_in_jvm` to show the setup page (`url`) in a WebView
/// popup. The popup is built on Android's main thread by `SetupPage` (src/android/java): views
/// belong to that thread, which delivers configuration changes such as rotation or folding.
pub fn show_webview_popup(env: &mut JNIEnv, android_app: &AndroidApp, url: &str) {
    let activity = unsafe { JObject::from_raw(android_app.activity_as_ptr() as jobject) };
    let result = (|| -> jni::errors::Result<()> {
        let loader = env
            .call_method(
                &activity,
                "getClassLoader",
                "()Ljava/lang/ClassLoader;",
                &[],
            )?
            .l()?;
        let name = env.new_string("io.github.localdesktop.SetupPage")?;
        let class = env
            .call_method(
                &loader,
                "loadClass",
                "(Ljava/lang/String;)Ljava/lang/Class;",
                &[(&name).into()],
            )?
            .l()?;
        let url = env.new_string(url)?;
        env.call_static_method(
            JClass::from(class),
            "show",
            "(Landroid/app/Activity;Ljava/lang/String;)V",
            &[(&activity).into(), (&url).into()],
        )?;
        Ok(())
    })();
    if let Err(error) = result {
        log::error!("Failed to show the setup page: {error}");
        let _ = env.exception_describe();
        let _ = env.exception_clear();
    }
}
