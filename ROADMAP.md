# Roadmap: Local Desktop on Android

Where this fork is going, from the working notes in [docs/dev-notes.md](docs/dev-notes.md)
(dated session log; its last "CONSOLIDATED STATE" section is the current state). Design docs:
[architecture](docs/architecture.md), [Game Mode](docs/game-mode.md),
[Steam Deck controller](docs/steam-deck-controller.md).

## Goals
- Plasma Mobile on the phone, GPU-drawn (Turnip on KGSL, Zink), touch, keyboard, rotation, folding and display changes.
- Brave Origin Nightly and Firefox on the GPU.
- Steam: Valve's ARM64 client, x86 games through FEX, Windows games through Proton ARM64.
- Later: a desktop spanning the phone and external displays.

## Control baseline
Tag `control-2026-10-29-v2` (package `io.github.localdesktop`, APK `ld-20261029-m2-relative-paths`):
our work plus upstream's commits up to 0543368 except #328, upstream's own GPU stack. Paths follow the
package name, so test copies can be installed under other names. Every fix or
feature below starts on its own branch from it and is compared against it. (The earlier
`control-2026-10-29` predates the merge.)

What works there: Plasma Mobile with KWin GPU compositing and zero-copy dmabuf import; Brave and
Firefox on the GPU; Steam in Game Mode (gamescope nested in KWin, Steam Deck UI), with a progress
notification while Steam starts or updates; Bastion (x86, FEX); the emulated Steam Deck controller in
Steam's UI.

## Next: merge the graphics stacks (A/B done 2026-10-29)
Both stacks are installed side by side ("Local Desktop" = ours, "Local Desktop B" = upstream #328
on Xfce). With the same frame loop they perform the same; ours is required for Plasma Mobile (KWin
needs a GPU device). Upstream's frame loop helps KWin on the GPU (+40%) and hurts labwc on the CPU
(-1/3); its AHardwareBuffer upload showed no gain. Next: a hybrid frame loop measured on both
desktops, and the Xfce GPU fix (branch `fix/xfce-gpu`; labwc on the GPU still stalls on a fence).
Results and branches: docs/dev-notes.md, last CONSOLIDATED STATE.

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
- **Xfce on a fresh install can't start**: labwc finds the fake KGSL render node, Mesa can't name
  its driver, and labwc refuses software EGL. Share the session's GPU setup with Xfce.
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
