# The frame loop: hybrid C (control since 2026-10-30)

The frame loop decides when the app's compositor draws a frame onto the Android window, and when
it tells the guest compositor (KWin for Plasma Mobile, labwc for Xfce) that it may start its next
one, by sending Wayland frame callbacks. That timing sets the frame-rate ceiling of everything in
the session. On 2026-10-30 the hybrid loop "C" became the control, after an A/B/C test that
compared it with our old loop and with upstream's (results below).

## The three loops that were compared

| | Wakes up on | Draws | Frame callbacks go out |
|---|---|---|---|
| **A**: old control (`wake.rs`) | its own wake channel | on each commit, after swapping the buffer | after our frame is drawn ("redraw" release) |
| **3a**: upstream #328's loop on our GPU stack | client requests (`watcher.rs`), vsync (`frame_clock.rs`, Android's `AChoreographer`) | at most once per display refresh; sleeps when idle | at the next vsync ("vsync" release) |
| **C**: hybrid = 3a + one rule | as 3a | as 3a | right after a commit, if a refresh has passed since the last release ("commit" release); otherwise at the next vsync |

Why neither A nor 3a was enough:
- **A on Plasma:** KWin draws fast on the GPU, but it only gets its callback once our frame is
  drawn and swapped. It then needs ~16 ms to produce the next frame, which caps Plasma at about
  60–70 fps.
- **3a on Xfce:** labwc composites on the CPU and needs ~26 ms per frame. Holding its callback
  until the next vsync adds up to a refresh of idle time per frame, so Xfce drops from ~38 to ~25
  fps.
- **C:** releasing on commit lets a slow producer keep its own pace, and the once-per-refresh
  limit keeps a fast one at the display's rate. frame-stats' `cb[redraw/vsync/commit]` counters
  show which rule applied. In the tests: Xfce 0/0/452 (all on commit, like A); Plasma about
  0/800/300 (mostly vsync, like 3a).

The rule lives in `dispatch_clients` and `release_frame_callbacks`
(`src/android/backend/wayland/event_handler.rs`), with `WaylandBackend::callbacks_released_at`.
Everything else in our GPU stack is unchanged: linux-dmabuf, the KGSL render node, zero-copy
import, presentation feedback and frame stats.

## What the merge brought in (merge commit "Merge hybrid frame loop C: the new control")

- `496f074`: upstream #328's frame loop on our GPU stack (3a): `watcher.rs`, `frame_clock.rs`,
  idle; `wake.rs` removed.
- `98a8303`: the hybrid release rule (C).
- `3c86924`: frame-stats frame-callback timing (`cb[...]`: release counts, commit→release wait,
  release→next-commit produce time).
- Branch `fix/xfce-gpu`:
  - `6cecc39`: one GPU environment for both sessions (`/usr/local/lib/localdesktop-gpu-env.sh`).
  - `a9e9ad5`: kgsl-drm-shim answers `DRM_IOCTL_AUTH_MAGIC` with EACCES.
  - `19a30ff`: kgsl-drm-shim reports `DRM_CAP_PRIME`.
  - `85a427b`: `SHELL_PROC` in the device tools, so they also work on Xfce.
- **Left out:** experiment 3b (upstream's AHardwareBuffer upload for shared-memory frames). It
  showed no measurable gain.

## One app, one APK

From now on there is a single build: package `io.github.localdesktop`, label "Local Desktop",
built from `main`. It is the only Local Desktop app
installed on the test phone. The A/B/C test slot (`app.polarbear`, "Local Desktop B"/"C") is
retired: it was a one-commit relabel on top of each experiment branch, and it has been uninstalled.

- Control tag: `control-2026-10-30-hybrid-c`.
- APK: `ld-20261030-a-hybrid-control`.
- Check after installing it over the old control (data and Steam sign-in kept): Plasma Mobile up,
  SurfaceFlinger 104 fps, es2gears 114–119 fps, mixed vsync/commit releases, 0 crashes.

## A/B/C test results (2026-10-30)

**Setup.** Each build was installed in turn in the test slot on the same phone (120 Hz display,
plugged in, kept awake). Each run: 3 cold starts, `ld-framebench` (es2gears plus an app-drawer
pass), then `ld-shadejank` with Brave playing a YouTube video. 3 rounds were run round-robin, so
heat and drift spread evenly: 29 valid runs, 0 crashes. Raw data is local
(`~/localdesktop-bench/abc-next/results-20261030-0843`, `results-xfce-20261030-1111`).

**Columns.**
- **SF fps:** frames per second that Android's SurfaceFlinger received from the app window, while
  es2gears (an OpenGL demo that draws as fast as it is allowed) runs in the session. This is the
  frame-rate ceiling of the whole chain: client → guest compositor → our compositor → screen.
- **Shade jank:** the share of late frames in Android's own quick-settings shade, pulled down and
  up 10 times while Brave plays the video. It measures how much the busy session disturbs
  Android's UI, not how smooth Brave is.
- **CPU:** CPU time of all of the app's processes during the shade test, in % of one core. Noisy:
  ±20 points between rounds of the same config.

| Average of 3 rounds | Plasma SF fps | Plasma es2gears | Plasma shade jank | Plasma CPU | Xfce SF fps | Xfce shade jank | Xfce CPU |
|---|---|---|---|---|---|---|---|
| **C: hybrid (new control)** | **101.0** (93/103/107) | **113.0** | 14.3% | 152% | **38.5** | 14.4% | 162% |
| 3a: upstream loop | 103.0 (105/106/98) | 111.2 | 13.6% | 147% | 25.4 | 13.5% | 174% |
| A: old control | 66.1 (57/69/73) | 61.6 | 14.1% | 134% | 38.7 | 12.9% | 152% |
| B: upstream #328 as shipped | (KWin quits: no GPU device) | | | | 24.8 | 14.3% | 163% |

Cold starts didn't differ between loops: about 7 s for Plasma, 5 s for Xfce.

**Conclusions.**
- C matches 3a on Plasma (≈1.5× the old control) and matches the old control on Xfce, where 3a and
  upstream lose a third. It is the only loop that is fast on both desktops.
- The one cost seen is shade jank 0.7–1.5 points higher than the best loop on each desktop, in
  every round: small but consistent (open item below).

**Caveats.**
- *The Xfce configs* (session overrides used only for the test): labwc composited on the CPU
  (`WLR_RENDERER=pixman`) in all of them. For our stack, apps also ran with
  `LIBGL_ALWAYS_SOFTWARE=1 MESA_VK_WSI_DEBUG=sw`, so es2gears probably drew on the CPU too (the
  renderer wasn't recorded). Every loop ran the same config, so the comparison holds, but Xfce's
  absolute numbers are a CPU-path ceiling, not a GPU one.
- *Tooling fix during the run:* the device tools on the old control branch had no `SHELL_PROC`, so
  es2gears and Brave never started on Xfce in rounds 1–2 of the first pass. Those rounds were
  re-run with the fixed tools; only the valid runs are counted.
- *Interruptions:* two Google sign-in prompts landed on cold starts (excluded) and never on a
  benchmark phase. Every install waited for a Play Protect confirmation on the phone, which delays
  a run but doesn't affect its measurements.

## Open items

1. **Shade jank under C** is ~1 point higher. A likely cause is the extra commit releases, which
   let Brave and KWin draw more often while the shade animates. Look at it with frame-stats and
   the shade test before tuning the rule (e.g. stop commit releases while an Android overlay is
   up).
2. **Xfce default on a fresh install:** after the merge, labwc composites on the GPU by default,
   but on the GPU it still stalls on a fence in its first frame. Plasma Mobile (the shipped
   session config) is unaffected. Until the stall is fixed, Xfce needs `WLR_RENDERER=pixman`.
3. Record the GL renderer in `ld-framebench`, so CPU and GPU client paths can't be confused.
