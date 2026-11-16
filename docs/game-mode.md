# Steam Game Mode

The Steam entry in the app menu opens Game Mode: Steam's Steam Deck UI inside gamescope, nested in
KWin and fullscreen, as SteamOS runs it. DroidDeck's `droiddeck-session` (GPL-3.0) was the
reference throughout.

## Pieces

| Piece | Where | Does |
|---|---|---|
| `localdesktop-gamemode` | guest/managed/usr/local/bin | Starts gamescope with `localdesktop-steam --gamemode`, or brings a running Game Mode forward. Writes gamescope's pid to `/run/localdesktop-pad/gamemode` (the process is named `gamescope-wl`, so `pgrep -x gamescope` does not find it). |
| `localdesktop-steam --gamemode` | same | Steam with `-gamepadui -steamdeck -steamos3`; `--shutdown`, `--restart`, `--desktop` for the other cases. |
| `localdesktop-steam-compat` | same | Run by `localdesktop-steam` before each client start: maps every Linux game without an arm64 build to "Linux x86 (FEX)" (`localdesktop-fex`) in config.vdf, keeping any tool the user picked. Tests: `guest/tools/steam-compat-test.py`. |
| `steamos-session-select` | same | What the Deck UI runs for "Switch to Desktop": shuts Steam down, so gamescope ends. Untested. |
| gamescope 3.16.29 | guest/pkgbuilds/gamescope, shipped as a package in guest/blobs/packages | Arch's packaging, DroidDeck's 19 patches, and ours (0123: Steam's own screen follows the window; 0124: a game at the native size follows it too; 0200: a nested backend needs no DRM primary node). |
| Turnip render-node patch | patches/mesa/0001, guest/build-turnip.sh | Turnip on KGSL reports the bound `/dev/dri/renderD0` through `VK_EXT_physical_device_drm`. |

## Why each part is there

- **GPU buffers into gamescope.** gamescope finds its render node through
  `VK_EXT_physical_device_drm`. Turnip on KGSL did not report one, so gamescope offered its clients
  no dma-buf and frames came in as software copies (`MESA_VK_WSI_DEBUG=sw`, Xwayland without
  glamor), shown out of order: vkcube shook, gears flashed. With the Turnip patch frames arrive as
  GPU buffers and play smoothly. gamescope then refused to start without a primary node, which the
  nested backend never uses; patch 0200 only asks for one when the backend is session-based (DRM).
- **Screen size.** The launcher sets no sizes: gamescope's window takes the screen KWin gives it,
  and `--xwayland-count 2` makes gamescope keep Steam's X server equal to that window through
  rotations and folds. Games run on the second X server. With only one X server, Steam's screen
  stays at the size fixed at launch (1280x720 with no flags).
- **Steam's own screen stays full size.** On a screen larger than 1920x1080, Steam asks gamescope
  (`GAMESCOPE_XWAYLAND_MODE_CONTROL`) to shrink its own X server to 1920x1080, and gamescope clamps
  that to the window (1811x1080 upright on a 2176x1811 screen). Steam's "Native" game resolution
  reads that screen's size back, so games shrank too. Patch 0123, turned on by the launcher with
  `GAMESCOPE_STEAM_SCREEN_FOLLOWS_WINDOW=1`, ignores those requests for server #0 and acknowledges
  them by deleting the property, as gamescope does for a request that needs no change. Unacknowledged,
  Steam repeats the request and then crashes. Requests for the game servers are applied as before.
  No size is fixed anywhere: "Native" is whatever screen Game Mode is on (inner or cover screen,
  any rotation, an external display).
- **Games follow rotation.** gamescope updates only Steam's X server when the window changes, so
  a game started at the native size stayed at the old size after a rotation or fold (letterboxed
  or cropped). Patch 0124, turned on with `GAMESCOPE_NATIVE_GAMES_FOLLOW_OUTPUT=1`, resizes each
  game server whose size equals the old window to the new one; a game Steam gave its own size
  (Bastion's 1920x1080) keeps it. The rule is in `src/Utils/NativeGameFollow.h`, tested by
  `tests/test_native_game_follow.cpp`.
- **x86 Linux games.** The arm64 client runs them through Valve's FEX tool, which needs a guest
  rootfs (`/usr/share/guestos/fex-mesa`) Local Desktop doesn't have, so they didn't start (Golf
  With Your Friends). Our "Linux x86 (FEX)" tool has its own rootfs (the Steam Linux Runtime, with
  GL and Vulkan thunks). `localdesktop-steam-compat` makes it the default for each x86-only Linux
  game Steam knows (from `appcache/appinfo.vdf`), as a per-app mapping. A game mapped to another
  tool keeps it, and a game the user unmapped stays unmapped. The client rewrites config.vdf when
  it exits, so the mapping is written only before it starts; a game bought while Steam runs is
  mapped at the next start.
- **Game resolution.** Games default to Steam's global "Native", the full screen. A game that
  only draws at 16:9 keeps a per-game Game Resolution of 1920x1080 (see below).
- **Touch.** gamescope's `--default-touch-mode 4` (passthrough) is for the Steam UI; Steam sets the
  per-game mode (`STEAM_TOUCH_CLICK_MODE` on gamescope's X root, Left click by default for games).
- **Opening.** gamescope opens its window only once Steam draws, 10 s to a minute after the tap,
  when Plasma Mobile no longer treats it as the app just opened. A KWin script loaded for the
  session brings each new gamescope window to the front, and a notification covers the wait.
- **Steam's client channel** follows Steam's own `package/beta` (localdesktop-steam-install):
  forcing another channel made the client reinstall itself on every start.

## Not working yet

- The on-screen gamepad (`src/android/backend/wayland/gamepad.rs`) is off unless
  `/root/.config/localdesktop/gamepad-enabled` exists: it stopped being drawn while its controls
  still took touches, Steam's menu did not appear for its STEAM button, and touching it made the
  screen flash. See steam-deck-controller.md for its requirements.
- Games that letterbox themselves misplace touches by their own bars, which nothing below the
  game can correct. Bastion (decompiled) sizes its backbuffer to the screen, draws a 16:9 viewport
  of the full width centred vertically, and reads the mouse without subtracting the viewport's
  offset: on a taller screen touches land low by the top bar (~293 px at 2176x1811), on a wider one
  it crops the picture and touch is off the other way. Only an exact 16:9 game screen is right, so
  Bastion keeps Steam's per-game Game Resolution of 1920x1080: gamescope scales it and adds the
  bars, and touch is exact. Steam's list offers no larger 16:9 size, and a fixed custom size would
  suit one screen only; for such games this is the intended setup.
- Once, an instance stopped turning taps into clicks (the cursor moved, Xwayland got no button
  events); a fresh instance clicked. To trace it: start Game Mode with `WAYLAND_DEBUG=1` and look for
  `wl_touch.down` from KWin and `wl_pointer.button` sent to Xwayland in /tmp/gamemode.log.
- Games do not yet see Steam Input's virtual pad.
