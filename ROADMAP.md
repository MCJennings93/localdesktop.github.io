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

## Current state
`main` is the app: one build (package `io.github.localdesktop`, "Local Desktop"), up to date with
upstream plus this fork's work. New work goes on a short-lived branch and merges back into `main`.

What works: Plasma Mobile with KWin GPU compositing and zero-copy dmabuf import, at ~100 fps; Brave
and Firefox on the GPU; Steam in Game Mode (gamescope nested in KWin, Steam Deck UI), with a
progress notification while Steam starts or updates; Bastion (x86, FEX); the emulated Steam Deck
controller in Steam's UI; Brave as the default browser (Firefox if Brave isn't installed); Chinese,
Japanese and Korean text, with the right glyphs per language (Noto CJK).

## Order of work (set 2026-10-30)

### 1. Short fixes, in this order
1. **GL renderer in `ld-framebench`.** Record the renderer es2gears runs on, so a CPU path can't be
   mistaken for a GPU one (the Xfce figures in [docs/frame-loop.md](docs/frame-loop.md)).
2. **DNS refresh.** The app writes the guest's `resolv.conf` from Android's DNS only at start; it
   should follow network changes (Wi-Fi ↔ mobile data, VPN).

### 2. App process lifecycle
**Keep running in the background.** When the app is minimized and the phone is used for something
else, the session keeps running and nothing closes or crashes. A browser playing a YouTube video
keeps playing (audio continues); a game keeps running or pauses on its own, and is intact on
return.

Done:
- A foreground service with a notification ("Close desktop") keeps the app and its guest processes
  alive; swiping the app from recents ends the session.
- Audio keeps running while hidden, and the AAudio sink reopens its stream when the output changes.
- Clients keep drawing while the window is hidden.
- A session that dies is noticed: the app restarts it if visible and it had been up for a while,
  and otherwise says so in the notification. Leftover guest processes are cleaned up.
- `ld-backgroundtest` passes: 120 s hidden with audio at 100% and the session intact. A real-world
  test on battery was reasonably stable.

Open:
- **Hidden draw rate.** Clients through KWin draw about 100 fps while hidden, not the intended 10 or
  so. The battery cost hasn't been measured.
- **Not yet verified on the phone:** switching audio output after the fix, a real activity destroy,
  a session dying while hidden, screen off, a longer hide, real video in Brave, and the "Close
  desktop" button.
- **Surviving an activity destroy.** The process exits with the activity, which ends the session;
  keeping it needs the compositor off `android_main`.
- **Android can still kill guest processes:** the phantom process killer needs "Disable child
  process restrictions" on the phone. There is no wake lock, so the CPU may sleep with the screen off
  unless audio holds it.

### 3. Compositor
1. **Shade jank under the frame loop:** about 1 point higher than with the best loop on each
   desktop ([docs/frame-loop.md](docs/frame-loop.md)).
2. **labwc stalls on a fence on the GPU** in its first frame, so Xfce on a fresh install needs
   `WLR_RENDERER=pixman` for now. Fix the stall in the sync layer (`kgsl-syncobj`,
   `kgsl-drm-shim`).
3. Later features, both designed in `docs/architecture.md`:
   - **Multiple displays** (KWin virtual outputs per Android display).
   - **Two-way notification bridge** (Linux ↔ Android shade).

### 4. Steam and games
1. **Controller input:**
   - **Physical controllers.** A controller paired with Android (e.g. a Switch Pro Controller)
     becomes the Steam Deck controller Steam reads: gamepad events from winit instead of keys and
     mouse motion, merged into the pad ring.
   - **Games see Steam Input's virtual pad.** Needs x86-64 libfakeinput in the FEX rootfs, plus
     evdev sysfs.
   - **On-screen gamepad** (off by default). It stops drawing while still taking touches, its STEAM
     button doesn't open Steam's menu or the Quick Access Menu, and touching it flashes the screen.
2. **Games at any resolution and aspect ratio.** Games that letterbox themselves (Bastion at
   non-16:9 sizes) misplace touch by their own bars. Options: per-game Game Resolution, windowed
   mode inside gamescope, a newer FNA.
3. **Clicks stopped in one Game Mode instance.** The cursor moved but no button events arrived.
   Seen once. Guess: stale pointer focus after a game changed resolution. Trace recipe in the notes.
4. **Proton ARM64 and Windows games.** DroidDeck's recipes cover this: VKD3D/DXVK defaults, SysV
   IPC shim, tracer hiding, the pressure-vessel probe shim.
5. Smaller items:
   - "Switch to Desktop" is untested.
   - Steam's menu can't be reached by touch.
   - Plasma Mobile may not show .desktop Actions (Restart Steam).
   - A real Steam update hasn't been seen through the progress notification yet (only replays).

## Backlog (unordered)
- **Green or grayscale video frames in Chromium/Brave.** Worked around with
  `--disable-gpu-memory-buffer-video-frames`; the root cause is open. The notes give two tests:
  memory pressure, and a Chromium trace.
- **Zero-copy options 2 and 3** (Turnip inside the app; host-allocated AHardwareBuffers). These
  make it more robust if an Adreno driver update breaks today's metadata trick.
- From the Droid Deck review: preload shims (`robust`, `usr2`, `writewatch` for Wine, `kgslid`,
  `net`/`netif`, `pulseguard`) and the AAudio sinks if audio problems show up.
- Small items:
  - Firefox content processes segfault ("Shared memory PlatformHandle is not safe to map").
  - The guest overlay never deletes files dropped from the APK.
  - Drop zink-kopper-shim and `LIBGL_ALWAYS_SOFTWARE` once the QPainter path is gone.

## Working rules
- Fix the shared layer instead of adding per-app flags; prove each fix with a test (`~/bin/ld-*`
  tools, frame detectors, replays).
- After any change under `guest/managed`, build and install an APK before the app restarts: on
  every start the app rewrites those files from the installed APK.
