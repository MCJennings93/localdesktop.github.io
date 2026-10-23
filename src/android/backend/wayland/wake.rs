//! Wakes the event loop when a Wayland client has something for us (a new connection or
//! requests on an existing one), so the app only redraws when there is new content instead of
//! at every display refresh. A thread polls the listening socket and the display's fd, sends
//! `AppUserEvent::WaylandReady`, then waits until the event loop has dispatched (`rearm`), so
//! the still-readable fds don't make it spin.

use crate::android::accessibility::{event_loop_proxy, AppUserEvent};
use smithay::reexports::rustix::event::{poll, PollFd, PollFlags};
use std::os::fd::{BorrowedFd, RawFd};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

pub struct Waker {
    armed: Arc<(Mutex<bool>, Condvar)>,
}

impl Waker {
    /// Start watching `fds`, which must stay open for the life of the app (the compositor's
    /// listening socket and display fd do).
    pub fn spawn(fds: [RawFd; 2]) -> Waker {
        let armed = Arc::new((Mutex::new(true), Condvar::new()));
        let thread_armed = armed.clone();
        thread::Builder::new()
            .name("wayland-wake".into())
            .spawn(move || loop {
                {
                    let (lock, condvar) = &*thread_armed;
                    let mut armed = lock.lock().unwrap();
                    while !*armed {
                        armed = condvar.wait(armed).unwrap();
                    }
                }
                let (listener, display) =
                    unsafe { (BorrowedFd::borrow_raw(fds[0]), BorrowedFd::borrow_raw(fds[1])) };
                let mut pollfds = [
                    PollFd::new(&listener, PollFlags::IN),
                    PollFd::new(&display, PollFlags::IN),
                ];
                if poll(&mut pollfds, -1).is_err() {
                    continue;
                }
                *thread_armed.0.lock().unwrap() = false;
                if let Some(proxy) = event_loop_proxy() {
                    let _ = proxy.send_event(AppUserEvent::WaylandReady);
                }
            })
            .expect("Failed to spawn the Wayland wake thread");
        Waker { armed }
    }

    /// The event loop has dispatched the clients: watch the fds again.
    pub fn rearm(&self) {
        let (lock, condvar) = &*self.armed;
        *lock.lock().unwrap() = true;
        condvar.notify_one();
    }
}
