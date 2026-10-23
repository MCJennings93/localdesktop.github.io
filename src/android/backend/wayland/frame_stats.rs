//! Frame-timing counters for performance work, enabled by creating `frame-stats` in the app's
//! files directory (`run-as app.polarbear touch files/frame-stats`) before the app starts.
//! Every 2 s it logs how often the app redrew, how many new client buffers were committed (new
//! content, e.g. KWin frames), how fast those came while content was changing (`anim`: gaps
//! under 100 ms, i.e. during an animation, with a histogram of the gaps), and how long
//! rendering and the buffer swap took. Read with
//! `adb logcat | grep frame-stats`; SurfaceFlinger's `--latency` covers what reached the screen.

use crate::android::utils::application_context::get_application_context;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

const WINDOW: Duration = Duration::from_secs(2);

static ENABLED: OnceLock<bool> = OnceLock::new();
static COMMITS: AtomicU64 = AtomicU64::new(0);

/// Upper bounds (ms) of the buckets for gaps between new buffers during animations.
const GAP_BUCKETS: [u64; 7] = [9, 13, 18, 25, 34, 50, 100];

struct Gaps {
    last: Option<Instant>,
    count: u64,
    total: Duration,
    buckets: [u64; GAP_BUCKETS.len()],
}

static GAPS: Mutex<Gaps> = Mutex::new(Gaps {
    last: None,
    count: 0,
    total: Duration::ZERO,
    buckets: [0; GAP_BUCKETS.len()],
});

#[derive(Default)]
struct Window {
    start: Option<Instant>,
    redraws: u64,
    render: Duration,
    render_max: Duration,
    swap: Duration,
    swap_max: Duration,
}

static STATE: Mutex<Window> = Mutex::new(Window {
    start: None,
    redraws: 0,
    render: Duration::ZERO,
    render_max: Duration::ZERO,
    swap: Duration::ZERO,
    swap_max: Duration::ZERO,
});

pub fn enabled() -> bool {
    *ENABLED.get_or_init(|| get_application_context().data_dir.join("frame-stats").exists())
}

/// A client committed a new buffer.
pub fn buffer_committed() {
    if !enabled() {
        return;
    }
    COMMITS.fetch_add(1, Ordering::Relaxed);
    let now = Instant::now();
    let mut gaps = GAPS.lock().unwrap();
    if let Some(gap) = gaps.last.map(|last| now - last) {
        let ms = gap.as_millis() as u64;
        if let Some(bucket) = GAP_BUCKETS.iter().position(|&bound| ms < bound) {
            gaps.count += 1;
            gaps.total += gap;
            gaps.buckets[bucket] += 1;
        }
    }
    gaps.last = Some(now);
}

/// "anim=<fps> gaps[<9ms:n <13:n ...]" for the window, and resets it.
fn take_gaps() -> String {
    let mut gaps = GAPS.lock().unwrap();
    let fps = if gaps.count > 0 {
        gaps.count as f64 / gaps.total.as_secs_f64()
    } else {
        0.0
    };
    let buckets = GAP_BUCKETS
        .iter()
        .zip(gaps.buckets)
        .map(|(bound, n)| format!("<{bound}:{n}"))
        .collect::<Vec<_>>()
        .join(" ");
    gaps.count = 0;
    gaps.total = Duration::ZERO;
    gaps.buckets = [0; GAP_BUCKETS.len()];
    format!("anim={fps:.1}fps gaps-ms[{buckets}]")
}

/// One redraw: `render` is building and drawing the frame, `swap` is submitting it.
pub fn redrawn(render: Duration, swap: Duration) {
    if !enabled() {
        return;
    }
    let mut window = STATE.lock().unwrap();
    let now = Instant::now();
    let start = *window.start.get_or_insert(now);
    window.redraws += 1;
    window.render += render;
    window.render_max = window.render_max.max(render);
    window.swap += swap;
    window.swap_max = window.swap_max.max(swap);
    let elapsed = now - start;
    if elapsed < WINDOW {
        return;
    }
    let secs = elapsed.as_secs_f64();
    let n = window.redraws.max(1) as f64;
    let ms = |d: Duration| d.as_secs_f64() * 1000.0;
    log::info!(
        "frame-stats: redraws={:.1}/s new-buffers={:.1}/s {} render avg={:.2}ms max={:.2}ms swap avg={:.2}ms max={:.2}ms",
        window.redraws as f64 / secs,
        COMMITS.swap(0, Ordering::Relaxed) as f64 / secs,
        take_gaps(),
        ms(window.render) / n,
        ms(window.render_max),
        ms(window.swap) / n,
        ms(window.swap_max),
    );
    *window = Window {
        start: Some(now),
        ..Window::default()
    };
}
