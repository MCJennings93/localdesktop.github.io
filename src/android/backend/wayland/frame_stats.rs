//! Frame-timing counters for performance work, enabled by creating `frame-stats` in the app's
//! files directory (`run-as <package> touch files/frame-stats`) before the app starts.
//! Every 2 s it logs how often the app redrew, how many new client buffers were committed (new
//! content, e.g. KWin frames), how fast those came while content was changing (`anim`: gaps
//! under 100 ms, i.e. during an animation, with a histogram of the gaps), and how long
//! rendering and the buffer swap took. Read with
//! `adb logcat | grep frame-stats`; SurfaceFlinger's `--latency` covers what reached the screen.
//!
//! It also counts `green` frames: KWin frames (dmabufs the app copies) in which at least a tenth
//! of a 32x18 sample grid has no red or blue but some green. That is what a YUV video frame turns
//! into when its chroma reads as zero instead of 128: red and blue clamp to 0 and green becomes
//! the luma plus ~135 (BT.601; ~77 in BT.709), so the picture shows in green only, from (0,135,0)
//! where it is black up to (0,255,0) where it is bright. Brave's green flashes are this (a
//! screen recording of the grayscale test video, 2026-10-27). A tenth, not more, because a video
//! that is not fullscreen covers only part of the screen. Each such frame is also logged on its
//! own.
//!
//! For the synctest client (/root/synctest: solid frames cycling red, green, blue, slow to draw)
//! it counts `mixed` frames (more than one of the three colours: partly drawn when read) and
//! `stale` ones (the same colour as the previous synctest frame: read before the new one landed).

use crate::android::utils::application_context::get_application_context;
use smithay::backend::allocator::Fourcc;
use smithay::backend::renderer::gles::DMABUF_COPY_PROBE;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

const WINDOW: Duration = Duration::from_secs(2);

static ENABLED: OnceLock<bool> = OnceLock::new();
static COMMITS: AtomicU64 = AtomicU64::new(0);
static PROBED: AtomicU64 = AtomicU64::new(0);
static GREEN: AtomicU64 = AtomicU64::new(0);
static GREEN_TOTAL: AtomicU64 = AtomicU64::new(0);
static SYNC_FRAMES: AtomicU64 = AtomicU64::new(0);
static SYNC_MIXED: AtomicU64 = AtomicU64::new(0);
static SYNC_STALE: AtomicU64 = AtomicU64::new(0);
/// The colour (0 red, 1 green, 2 blue) of the last solid synctest frame, 3 if none.
static SYNC_LAST: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(3);

const GRID: (usize, usize) = (32, 18);
/// A frame counts as green when this share of the grid is (in percent).
const GREEN_PERCENT: usize = 10;

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
    *ENABLED.get_or_init(|| {
        let enabled = get_application_context().data_dir.join("frame-stats").exists();
        if enabled {
            let _ = DMABUF_COPY_PROBE.set(probe_frame);
        }
        enabled
    })
}

/// Samples a copied dmabuf frame for unfinished-video green (see the module docs).
fn probe_frame(pixels: &[u8], stride: usize, width: i32, height: i32, format: Fourcc) {
    // Byte offsets of red and blue in a little-endian 32-bit pixel; green is byte 1 in both.
    let (red, blue) = match format {
        Fourcc::Argb8888 | Fourcc::Xrgb8888 => (2, 0),
        Fourcc::Abgr8888 | Fourcc::Xbgr8888 => (0, 2),
        _ => return,
    };
    let (width, height) = (width.max(0) as usize, height.max(0) as usize);
    if width == 0 || height == 0 {
        return;
    }
    let mut green = 0;
    // Green samples below saturation: a video's picture always has some, synctest's solid pure
    // green frames have none.
    let mut shaded_green = 0;
    // Samples that are synctest's pure red, green or blue.
    let mut primaries = [0usize; 3];
    for row in 0..GRID.1 {
        let y = (2 * row + 1) * height / (2 * GRID.1);
        for column in 0..GRID.0 {
            let x = (2 * column + 1) * width / (2 * GRID.0);
            let Some(pixel) = pixels.get(y * stride + x * 4..y * stride + x * 4 + 4) else {
                return;
            };
            let (r, g, b) = (pixel[red], pixel[1], pixel[blue]);
            if r < 16 && b < 16 && g >= 48 {
                green += 1;
                if g < 240 {
                    shaded_green += 1;
                }
            }
            match (r > 230, g > 230, b > 230, r < 25, g < 25, b < 25) {
                (true, _, _, _, true, true) => primaries[0] += 1,
                (_, true, _, true, _, true) => primaries[1] += 1,
                (_, _, true, true, true, _) => primaries[2] += 1,
                _ => {}
            }
        }
    }
    PROBED.fetch_add(1, Ordering::Relaxed);
    let samples = GRID.0 * GRID.1;
    probe_synctest(primaries, samples);
    if green * 100 >= samples * GREEN_PERCENT && shaded_green > 0 {
        GREEN.fetch_add(1, Ordering::Relaxed);
        let total = GREEN_TOTAL.fetch_add(1, Ordering::Relaxed) + 1;
        log::warn!(
            "frame-stats: GREEN frame #{total}: {}% of samples ({width}x{height})",
            green * 100 / samples
        );
    }
}

/// A client committed a new buffer.
/// Why frame callbacks went out: with a redraw (the app's own loop), on a display refresh, or as
/// soon as a commit was taken.
#[derive(Clone, Copy)]
pub enum Release {
    Redraw,
    Vsync,
    Commit,
}

/// Frame-callback timing: how long a client's new frame waited for its callbacks (commit to
/// release), and how long the client took to produce the next frame (release to commit).
#[derive(Default)]
struct Callbacks {
    last_release: Option<Instant>,
    /// The first commit since the last release, which the next release answers.
    pending_commit: Option<Instant>,
    released: [u64; 3],
    wait: Duration,
    wait_max: Duration,
    waits: u64,
    produce: Duration,
    produces: u64,
}

static CALLBACKS: Mutex<Callbacks> = Mutex::new(Callbacks {
    last_release: None,
    pending_commit: None,
    released: [0; 3],
    wait: Duration::ZERO,
    wait_max: Duration::ZERO,
    waits: 0,
    produce: Duration::ZERO,
    produces: 0,
});

/// Frame callbacks went out to the clients' surfaces.
pub fn callbacks_released(why: Release) {
    if !enabled() {
        return;
    }
    let now = Instant::now();
    let mut cb = CALLBACKS.lock().unwrap();
    cb.released[why as usize] += 1;
    if let Some(commit) = cb.pending_commit.take() {
        let wait = now - commit;
        cb.wait += wait;
        cb.wait_max = cb.wait_max.max(wait);
        cb.waits += 1;
    }
    cb.last_release = Some(now);
}

fn callback_commit(now: Instant) {
    let mut cb = CALLBACKS.lock().unwrap();
    if cb.pending_commit.is_none() {
        cb.pending_commit = Some(now);
        if let Some(release) = cb.last_release {
            cb.produce += now - release;
            cb.produces += 1;
        }
    }
}

fn take_callbacks() -> String {
    let mut cb = CALLBACKS.lock().unwrap();
    let ms = |d: Duration, n: u64| d.as_secs_f64() * 1000.0 / n.max(1) as f64;
    let text = format!(
        "cb[redraw={} vsync={} commit={} wait avg={:.2}ms max={:.2}ms produce avg={:.2}ms]",
        cb.released[Release::Redraw as usize],
        cb.released[Release::Vsync as usize],
        cb.released[Release::Commit as usize],
        ms(cb.wait, cb.waits),
        ms(cb.wait_max, 1),
        ms(cb.produce, cb.produces),
    );
    cb.released = [0; 3];
    cb.wait = Duration::ZERO;
    cb.wait_max = Duration::ZERO;
    cb.waits = 0;
    cb.produce = Duration::ZERO;
    cb.produces = 0;
    text
}

pub fn buffer_committed() {
    if !enabled() {
        return;
    }
    COMMITS.fetch_add(1, Ordering::Relaxed);
    let now = Instant::now();
    callback_commit(now);
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

/// Classifies a frame that is (mostly) synctest output; see the module docs.
fn probe_synctest(primaries: [usize; 3], samples: usize) {
    let total: usize = primaries.iter().sum();
    if total * 10 < samples * 6 {
        return;
    }
    SYNC_FRAMES.fetch_add(1, Ordering::Relaxed);
    let present = primaries.iter().filter(|&&n| n * 50 >= samples).count();
    if present > 1 {
        SYNC_MIXED.fetch_add(1, Ordering::Relaxed);
        log::warn!("frame-stats: synctest MIXED frame r/g/b samples {primaries:?}");
        SYNC_LAST.store(3, Ordering::Relaxed);
        return;
    }
    let colour = (0..3).max_by_key(|&i| primaries[i]).unwrap_or(0) as u8;
    if SYNC_LAST.swap(colour, Ordering::Relaxed) == colour {
        SYNC_STALE.fetch_add(1, Ordering::Relaxed);
    }
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
        "frame-stats: redraws={:.1}/s new-buffers={:.1}/s {} {} probed={} green={} (total {}) sync[frames={} mixed={} stale={}] render avg={:.2}ms max={:.2}ms swap avg={:.2}ms max={:.2}ms",
        window.redraws as f64 / secs,
        COMMITS.swap(0, Ordering::Relaxed) as f64 / secs,
        take_gaps(),
        take_callbacks(),
        PROBED.swap(0, Ordering::Relaxed),
        GREEN.swap(0, Ordering::Relaxed),
        GREEN_TOTAL.load(Ordering::Relaxed),
        SYNC_FRAMES.swap(0, Ordering::Relaxed),
        SYNC_MIXED.swap(0, Ordering::Relaxed),
        SYNC_STALE.swap(0, Ordering::Relaxed),
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
