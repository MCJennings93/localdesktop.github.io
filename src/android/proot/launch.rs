use super::process::ArchProcess;
use crate::android::accessibility::{event_loop_proxy, AppUserEvent};
use crate::android::utils::application_context::get_application_context;
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

static LAUNCH_RUNNING: AtomicBool = AtomicBool::new(false);

const SESSION_LOG: &str = "/tmp/localdesktop-session.log";

/// A session that ends sooner than this is failing, not one that ran and got killed.
const EARLY_END: Duration = Duration::from_secs(30);

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
        let guard = LaunchRunningGuard;

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

        let started = Instant::now();
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
        kill_leftovers();
        drop(guard);
        if let Some(proxy) = event_loop_proxy() {
            let _ = proxy.send_event(AppUserEvent::SessionEnded {
                early: started.elapsed() < EARLY_END,
            });
        }
    });
}

/// Guest programs can outlive a killed session, orphaned and sometimes spinning. Everything under
/// this uid other than the app itself belongs to the session.
fn kill_leftovers() {
    let own_pid = std::process::id();
    let own_uid = unsafe { libc::getuid() };
    let Ok(entries) = fs::read_dir("/proc") else {
        return;
    };
    let mut killed = 0;
    for entry in entries.flatten() {
        let Some(pid) = entry.file_name().to_str().and_then(|it| it.parse::<u32>().ok()) else {
            continue;
        };
        if pid != own_pid && entry.metadata().is_ok_and(|it| it.uid() == own_uid) {
            unsafe { libc::kill(pid as libc::pid_t, libc::SIGKILL) };
            killed += 1;
        }
    }
    log::info!("Killed {killed} leftover guest processes");
}
