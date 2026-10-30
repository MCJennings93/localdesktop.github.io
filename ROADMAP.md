# Roadmap: Local Desktop on Android

Where this fork is going, from the working notes in [docs/dev-notes.md](docs/dev-notes.md)
(dated session log; its last "CONSOLIDATED STATE" section is the current state). Design docs:
[architecture](docs/architecture.md), [frame loop](docs/frame-loop.md), [Game Mode](docs/game-mode.md),
[Steam Deck controller](docs/steam-deck-controller.md).

## Goals
- Plasma Mobile on the phone, GPU-drawn (Turnip on KGSL, Zink), touch, keyboard, rotation, folding and display changes.
- Brave Origin Nightly and Firefox on the GPU.
- Steam: Valve's ARM64 client, x86 games through FEX, Windows games through Proton ARM64.
- Later: a desktop spanning the phone and external displays.

## Control baseline
Tag `control-2026-10-30-hybrid-c` (package `io.github.localdesktop`, APK
`ld-20261030-a-hybrid-control`): the previous control `control-2026-10-29-v2` plus the hybrid frame
loop C and the Xfce GPU environment ([docs/frame-loop.md](docs/frame-loop.md)). This is the only APK
built and the only Local Desktop app installed on the test phone; the A/B test slot
(`app.polarbear`) is retired. Every fix or feature below starts on its own branch from the tag and
is compared against it.

What works there: Plasma Mobile with KWin GPU compositing and zero-copy dmabuf import, at ~100 fps
(the old loop managed ~66); Brave and Firefox on the GPU; Steam in Game Mode (gamescope nested in
KWin, Steam Deck UI), with a progress notification while Steam starts or updates; Bastion (x86,
FEX); the emulated Steam Deck controller in Steam's UI.

## Graphics: stacks merged (A/B/C done 2026-10-30)
The hybrid loop is as fast as upstream #328's loop on Plasma (101 vs 103 fps) and as fast as our old
loop on Xfce (38.5 vs 38.7; upstream's loop 25). Upstream's AHardwareBuffer upload (3b) was dropped:
no gain. Open, in [docs/frame-loop.md](docs/frame-loop.md): Android's shade janks ~1 point more under
the hybrid; labwc on the GPU still stalls on a fence (Xfce needs `WLR_RENDERER=pixman` for now);
`ld-framebench` should record the GL renderer.

## Keep running in the background (added 2026-10-30)
Goal: when the app is minimized and the phone is used for something else, the session keeps
running and nothing closes or crashes. A browser playing a YouTube video keeps playing (audio
continues); a game keeps running or pauses on its own, and is intact on return.

What is known today:
- While the app is hidden, the compositor drops its GPU renderer and stops sending frame
  callbacks, so clients stop drawing until the app is visible again (`schedule_frame` in
  `event_handler.rs`). Video and games may stall on that rather than keep going.
- Android can kill or throttle the app's processes in the background: the phantom process killer
  (seen 2026-10-27; the test phone has "Disable child process restrictions" on), battery
  optimization, and memory pressure.
- The session only starts while the app is in the foreground.

Likely parts: a foreground service with a notification, so Android keeps the app and its proot
children alive; frame callbacks (or presentation feedback) that keep pacing while hidden without
drawing to the window; audio that keeps running; a clean return when the window comes back. To do
first: a background test (`ld-*` tool) that minimizes the app during a video and a game for N
minutes, checks that audio keeps playing and that nothing crashes or restarts, and measures what
happens on return. Related: guest-death handling (backlog).

## Steam and Game Mode
The user picks the order.
1. **Clicks stopped in one Game Mode instance.** The cursor moved but no button events arrived.
   Seen once. Guess: stale pointer focus after a game changed resolution. Trace recipe in the notes.
2. **Games that letterbox themselves** (Bastion at non-16:9 sizes) misplace touch by their own bars.
   Options: per-game Game Resolution, windowed mode inside gamescope, a newer FNA.
3. **On-screen gamepad** (off by default). It stops drawing while still taking touches, its STEAM
   button doesn't open Steam's menu or the Quick Access Menu, and touching it flashes the screen.
4. **Physical controllers (tabled).** A controller paired with Android (e.g. a Switch Pro Controller)
   becomes the Steam Deck controller Steam reads: gamepad events from winit instead of keys and
   mouse motion, merged into the pad ring.
5. **Games see Steam Input's virtual pad.** Needs x86-64 libfakeinput in the FEX rootfs, plus evdev
   sysfs.
6. **Proton ARM64 and Windows games.** DroidDeck's recipes cover this: VKD3D/DXVK defaults, SysV
   IPC shim, tracer hiding, the pressure-vessel probe shim.
7. Smaller items: "Switch to Desktop" is untested; Steam's menu can't be reached by touch; Plasma
   Mobile may not show .desktop Actions (Restart Steam); see a real Steam update through the
   progress notification (so far only replays have been run).

## Backlog
- **Xfce on a fresh install**: the shared GPU environment (2026-10-30) gets labwc a renderer, but
  labwc on the GPU stalls on a fence in its first frame. Fix the stall, or default labwc to pixman.
- **Green or grayscale video frames in Chromium/Brave.** Worked around with
  `--disable-gpu-memory-buffer-video-frames`; the root cause is open. The notes give two tests:
  memory pressure, and a Chromium trace.
- **Two-way notification bridge** (Linux ↔ Android shade). Designed in `docs/architecture.md`.
- **Multiple displays** (KWin virtual outputs per Android display). Designed in
  `docs/architecture.md`.
- **Zero-copy options 2 and 3** (Turnip inside the app; host-allocated AHardwareBuffers). These
  make it more robust if an Adreno driver update breaks today's metadata trick.
- **Guest-death handling.** When Android kills proot, the app should notice, clean up the leftover
  guest processes and say so.
- From the Droid Deck review: preload shims (`robust`, `usr2`, `writewatch` for Wine, `kgslid`,
  `net`/`netif`, `pulseguard`) and the AAudio sinks if audio problems show up.
- Small items: Firefox content processes segfault ("Shared memory PlatformHandle is not safe to
  map"); DNS is read only at app start; CJK fonts are missing; the guest overlay never deletes files
  dropped from the APK; a duplicate Brave drawer entry; Android's shade janks over the desktop; drop
  zink-kopper-shim and `LIBGL_ALWAYS_SOFTWARE` once the QPainter path is gone.

## Working rules
- Fix the shared layer instead of adding per-app flags; prove each fix with a test (`~/bin/ld-*`
  tools, frame detectors, replays).
- After any change under `guest/managed`, build and install an APK before the app restarts: on
  every start the app rewrites those files from the installed APK.
