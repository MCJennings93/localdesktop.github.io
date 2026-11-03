//! The Android foreground service that keeps the app alive while its window is hidden.
//! See `SessionService.java`.

use crate::android::proot::setup::write_resolv_conf;
use crate::core::config::arch_fs_root;
use jni::objects::{JClass, JObject, JObjectArray, JString, JValue};
use jni::sys::jobject;
use jni::JNIEnv;
use std::path::Path;
use winit::platform::android::activity::AndroidApp;

fn call(env: &mut JNIEnv, android_app: &AndroidApp, method: &str, signature: &str, args: &[JValue]) {
    let activity = unsafe { JObject::from_raw(android_app.activity_as_ptr() as jobject) };
    let result = (|| -> jni::errors::Result<()> {
        let loader = env
            .call_method(&activity, "getClassLoader", "()Ljava/lang/ClassLoader;", &[])?
            .l()?;
        let name = env.new_string("io.github.localdesktop.SessionService")?;
        let class = env
            .call_method(
                &loader,
                "loadClass",
                "(Ljava/lang/String;)Ljava/lang/Class;",
                &[(&name).into()],
            )?
            .l()?;
        let mut all = vec![JValue::from(&activity)];
        all.extend_from_slice(args);
        env.call_static_method(JClass::from(class), method, signature, &all)?;
        Ok(())
    })();
    if let Err(error) = result {
        log::warn!("SessionService.{method} failed: {error}");
        let _ = env.exception_clear();
    }
}

pub fn start(env: &mut JNIEnv, android_app: &AndroidApp) {
    call(env, android_app, "start", "(Landroid/app/Activity;)V", &[]);
}

pub fn session_ended(env: &mut JNIEnv, android_app: &AndroidApp, visible: bool, restarting: bool) {
    call(
        env,
        android_app,
        "sessionEnded",
        "(Landroid/app/Activity;ZZ)V",
        &[JValue::from(visible), JValue::from(restarting)],
    );
}

/// Android's default network changed, or its DNS servers did (Wi-Fi, mobile data, a VPN).
#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_SessionService_nativeDnsServersChanged(
    mut env: JNIEnv,
    _class: JClass,
    servers: JObjectArray,
) {
    let read = |env: &mut JNIEnv| -> jni::errors::Result<Vec<String>> {
        let mut all = Vec::new();
        for i in 0..env.get_array_length(&servers)? {
            let server = JString::from(env.get_object_array_element(&servers, i)?);
            all.push(env.get_string(&server)?.into());
        }
        Ok(all)
    };
    match read(&mut env) {
        Ok(servers) => write_resolv_conf(Path::new(arch_fs_root()), &servers),
        Err(error) => {
            log::warn!("Failed to read the new DNS servers: {error}");
            let _ = env.exception_clear();
        }
    }
}
