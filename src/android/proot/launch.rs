use super::process::ArchProcess;
use crate::android::utils::application_context::get_application_context;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;

static LAUNCH_RUNNING: AtomicBool = AtomicBool::new(false);

const SESSION_LOG: &str = "/tmp/localdesktop-session.log";

struct LaunchRunningGuard;

impl Drop for LaunchRunningGuard {
    fn drop(&mut self) {
        LAUNCH_RUNNING.store(false, Ordering::Release);
    }
}

pub fn launch() {
    if LAUNCH_RUNNING
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        log::info!("Skipping launch because the desktop session is already running");
        return;
    }

    thread::spawn(move || {
        let _guard = LaunchRunningGuard;

        // Clean up potential leftover files for display :1
        ArchProcess {
            command: "rm -f /tmp/.X1-lock".into(),
            user: None,
            log: None,
        }
        .run();
        ArchProcess {
            command: "rm -f /tmp/.X11-unix/X1".into(),
            user: None,
            log: None,
        }
        .run();

        let local_config = get_application_context().local_config;
        let username = local_config.user.username;

        // The session's output goes to a file in the guest, so a session that fails to start can
        // be diagnosed in any build (release builds don't log at trace level). The subshell keeps
        // any configured command valid, whatever it ends with.
        let output = ArchProcess {
            command: format!("(\n{}\n) >{SESSION_LOG} 2>&1", local_config.command.launch),
            user: Some(username),
            log: Some(Arc::new(|it| log::trace!("{}", it))),
        }
        .run();
        log::info!(
            "Desktop session ended ({}); its output is in {SESSION_LOG} in the guest",
            output.status
        );
    });
}
