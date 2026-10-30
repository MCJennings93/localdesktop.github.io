# The frame loop

The frame loop decides when the app's compositor draws a frame onto the Android window, and when
it tells the guest compositor (KWin for Plasma Mobile, labwc for Xfce) that it may start its next
one, by sending Wayland frame callbacks. That timing sets the frame-rate ceiling of everything in
the session.

## How it works

- A watcher thread (`watcher.rs`) wakes the event loop when clients send requests. Android's
  `AChoreographer` vsync clock (`frame_clock.rs`) paces redraws to at most one per display
  refresh. The loop sleeps when nothing changes.
- **Release rule:** when a client commits a new frame and at least one display refresh has passed
  since callbacks last went out, its frame callbacks go out right away, before the frame is drawn
  (a "commit" release). Otherwise the next vsync releases them (a "vsync" release).
- **Why:** a slow producer, such as labwc compositing on the CPU (~26 ms a frame), keeps its own
  pace instead of losing up to a refresh each frame. A fast one, such as KWin on the GPU, runs at
  the display's rate instead of waiting for our swaps.

The rule lives in `dispatch_clients` and `release_frame_callbacks`
(`src/android/backend/wayland/event_handler.rs`), with `WaylandBackend::callbacks_released_at`.
frame-stats' `cb[redraw/vsync/commit]` counters show which release applied: on Xfce they're all
commit releases, and on Plasma mostly vsync. The rest of the GPU stack is separate from the loop:
linux-dmabuf, the KGSL render node, zero-copy import, presentation feedback and frame stats.

## How it was chosen (test of 2026-10-30)

This loop replaced our earlier one after a side-by-side test against it and against upstream's.
The test used these labels:

| Test label | Loop | Frame callbacks go out |
|---|---|---|
| A | our earlier loop (`wake.rs`, now removed) | after our frame is drawn and swapped |
| 3a | upstream #328's loop, ported onto our GPU stack | at the next vsync |
| C | 3a plus the release rule above, **the loop now in `main`** | on commit if a refresh has passed, else at vsync |
| B | upstream #328 as shipped, with its own GPU stack | at the next vsync |

Why neither A nor 3a was enough:
- **A on Plasma:** KWin only got its callback after our swap, then needed ~16 ms to produce the
  next frame, which capped Plasma at about 60–70 fps.
- **3a on Xfce:** holding labwc's callback until the next vsync added idle time to every frame, so
  Xfce dropped from ~38 to ~25 fps.

Commits it brought into `main`:
- `496f074`: upstream #328's frame loop on our GPU stack.
- `98a8303`: the release rule.
- `3c86924`: frame-callback timing in frame-stats.
- Xfce on the GPU:
  - `6cecc39`: one GPU environment for both sessions (`/usr/local/lib/localdesktop-gpu-env.sh`).
  - `a9e9ad5`: kgsl-drm-shim answers `DRM_IOCTL_AUTH_MAGIC` with EACCES.
  - `19a30ff`: kgsl-drm-shim reports `DRM_CAP_PRIME`.
  - `85a427b`: `SHELL_PROC` in the device tools.
- Left out: upstream's AHardwareBuffer upload for shared-memory frames, which showed no gain.

### Method

- **Setup:** each build was installed in turn in a separate test copy of the app on the same phone
  (120 Hz display, plugged in, kept awake).
- **Each run:** 3 cold starts, `ld-framebench` (es2gears plus an app-drawer pass), then
  `ld-shadejank` with Brave playing a YouTube video.
- **Rounds:** 3 rounds round-robin, so heat and drift spread evenly. 29 valid runs, 0 crashes.
- **Raw data** is local: `~/localdesktop-bench/abc-next/results-20261030-0843` and
  `results-xfce-20261030-1111`.

What the columns mean:
- **SF fps:** frames per second that Android's SurfaceFlinger received from the app window while
  es2gears (an OpenGL demo that draws as fast as it is allowed) runs in the session. It is the
  frame-rate ceiling of the whole chain: client → guest compositor → our compositor → screen.
- **Shade jank:** the share of late frames in Android's own quick-settings shade, pulled down and
  up 10 times while Brave plays the video. It measures how much the busy session disturbs
  Android's UI, not how smooth Brave is.
- **CPU:** CPU time of all of the app's processes during the shade test, in % of one core. It is
  noisy, ±20 points between rounds of the same config.

### Results

| Average of 3 rounds | Plasma SF fps | Plasma es2gears | Plasma shade jank | Plasma CPU | Xfce SF fps | Xfce shade jank | Xfce CPU |
|---|---|---|---|---|---|---|---|
| **C: current loop** | **101.0** (93/103/107) | **113.0** | 14.3% | 152% | **38.5** | 14.4% | 162% |
| 3a: upstream's loop | 103.0 (105/106/98) | 111.2 | 13.6% | 147% | 25.4 | 13.5% | 174% |
| A: earlier loop | 66.1 (57/69/73) | 61.6 | 14.1% | 134% | 38.7 | 12.9% | 152% |
| B: upstream #328 | (KWin quits: no GPU device) | | | | 24.8 | 14.3% | 163% |

Cold starts didn't differ between loops: about 7 s for Plasma and 5 s for Xfce.

**Conclusions.**
- The current loop matches upstream's on Plasma (about 1.5× the earlier loop) and matches the
  earlier loop on Xfce, where upstream's loses a third. It is the only one fast on both desktops.
- Its one cost is shade jank 0.7–1.5 points higher than the best loop on each desktop, in every
  round. Small, but consistent.

**Caveats.**
- *Xfce configs:* labwc composited on the CPU (`WLR_RENDERER=pixman`) in all of them. For A, 3a
  and C, apps also ran with `LIBGL_ALWAYS_SOFTWARE=1 MESA_VK_WSI_DEBUG=sw`, so es2gears probably
  drew on the CPU too (the renderer wasn't recorded). Every loop ran the same config, so the
  comparison holds, but the Xfce numbers are the ceiling of a CPU path.
- *Tooling fix during the run:* the device tools on the earlier branch had no `SHELL_PROC`, so on
  Xfce es2gears and Brave didn't start in rounds 1–2 of the first pass. Those rounds were re-run;
  only valid runs are counted.
- *Interruptions:* two Google sign-in prompts landed on cold starts, which are excluded. None
  landed on a benchmark phase.

## Open items

1. **Shade jank:** about 1 point higher than the best loop on each desktop. A likely cause is the
   commit releases, which let Brave and KWin draw more often while the shade animates. Look at it
   with frame-stats and the shade test before tuning the rule (for example, no commit releases
   while an Android overlay is up).
2. **Xfce on a fresh install:** labwc composites on the GPU by default, but on the GPU it stalls on
   a fence in its first frame. Plasma Mobile is unaffected. Until the stall is fixed, Xfce needs
   `WLR_RENDERER=pixman`.
3. **Renderer in `ld-framebench`:** record the GL renderer, so CPU and GPU client paths can't be
   confused.
