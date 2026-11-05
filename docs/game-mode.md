# Steam Game Mode

The Steam entry in the app menu opens Game Mode: Steam's Steam Deck UI inside gamescope, nested in
KWin and fullscreen, as SteamOS runs it. DroidDeck's `droiddeck-session` (GPL-3.0) was the
reference throughout.

## Pieces

| Piece | Where | Does |
|---|---|---|
| `localdesktop-gamemode` | guest/managed/usr/local/bin | Starts gamescope with `localdesktop-steam --gamemode`, or brings a running Game Mode forward. Writes gamescope's pid to `/run/localdesktop-pad/gamemode` (the process is named `gamescope-wl`, so `pgrep -x gamescope` does not find it). |
| `localdesktop-steam --gamemode` | same | Steam with `-gamepadui -steamdeck -steamos3`; `--shutdown`, `--restart`, `--desktop` for the other cases. |
| `steamos-session-select` | same | What the Deck UI runs for "Switch to Desktop": shuts Steam down, so gamescope ends. Untested. |
| gamescope 3.16.29 | guest/pkgbuilds/gamescope, shipped as a package in guest/blobs/packages | Arch's packaging, DroidDeck's 19 patches, and ours (0200: a nested backend needs no DRM primary node). |
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
- **Touch.** gamescope's `--default-touch-mode 4` (passthrough) is for the Steam UI; Steam sets the
  per-game mode (`STEAM_TOUCH_CLICK_MODE` on gamescope's X root, Left click by default for games).
- **Opening.** gamescope opens its window only once Steam draws, 10 s to a minute after the tap,
  when Plasma Mobile no longer treats it as the app just opened. A KWin script loaded for the
  session brings each new gamescope window to the front, and a notification covers the wait.
- **Steam's client channel** follows Steam's own `package/beta` (localdesktop-steam-install):
  forcing another channel made the client reinstall itself on every start.

## Not working yet

- On screens larger than 1920x1080, Steam's X server is cut to a 1920x1080 request clamped to the
  window (1920x1080 sideways, 1811x1080 upright on a 2176x1811 screen), so Steam doesn't fit. Smaller
  screens fit exactly. The requester isn't known yet; removing Steam's saved `GameResolutionGlobal`
  didn't change it.
- The on-screen gamepad (`src/android/backend/wayland/gamepad.rs`) is off unless
  `/root/.config/localdesktop/gamepad-enabled` exists: it stopped being drawn while its controls
  still took touches, Steam's menu did not appear for its STEAM button, and touching it made the
  screen flash. See steam-deck-controller.md for its requirements.
- Games that letterbox themselves at a resolution other than the game screen's shape misplace
  touches by their own bars, which nothing below the game can correct (Bastion was ~300 px low at
  the display's shape). Steam's per-game Game Resolution works around it (1920x1080 for Bastion).
  Not yet rechecked since the launcher stopped forcing 16:9.
- Once, an instance stopped turning taps into clicks (the cursor moved, Xwayland got no button
  events); a fresh instance clicked. To trace it: start Game Mode with `WAYLAND_DEBUG=1` and look for
  `wl_touch.down` from KWin and `wl_pointer.button` sent to Xwayland in /tmp/gamemode.log.
- Games do not yet see Steam Input's virtual pad.
