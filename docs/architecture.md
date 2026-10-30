<!--
  This is the SPINE of the generated architecture PDF (see src/bin/build_docs.rs).
  The prose is curated; the code is not. Each snippet directive (the HTML-comment
  lines beginning with "snippet", expanded by build_docs) is replaced at build
  time with the named function pulled fresh from source, so the walkthrough can
  never drift out of sync with the code it describes.

  The order is deliberate: the program is flattened along its call stack rather
  than scanned file-by-file. We start at the one entry point the OS calls,
  android_main, follow the fork into the two backends, then cross over to the two
  ways the APK that contains all of this gets built.
-->

# `android_main` — the one entry point

Local Desktop is an Android [`NativeActivity`](https://developer.android.com/ndk/reference/group/native-activity): there is no Java `main`, no `onCreate` we write. The NDK glue calls a single exported symbol, `android_main`, on a dedicated thread, and everything in this document hangs off that call.

`android_main` does four things in order: wire up logging and crash reporting, capture the Android handles we need later, build the winit event loop, and hand control to it. The last two lines are the whole program in miniature — **Phase 1: Setup** decides *what* we are (installer or desktop), **Phase 2: Run** drives it forever.

<!--snippet file=src/android/main.rs fn=android_main-->

`PolarBearApp::build` is where the fork is decided. It calls `setup`, which returns a *backend* — and the backend it returns is the entire branch this run will take.

<!--snippet file=src/android/app/build.rs fn=build-->

The two backends are the two halves of this chapter. There is no third option: a run is either still *installing the Linux environment* (WebView) or *running the desktop* (Wayland).

<!--snippet file=src/android/app/build.rs enum=PolarBearBackend-->

`setup` is the dispatcher. It first rejects unsupported devices outright, then runs the install pipeline. The crucial design choice is that `setup` **never blocks**: each stage reports whether its work was already done on a previous launch. If every stage was already done, the device is ready and we return the `Wayland` backend immediately. If any stage still has work to do, that work is spawned onto a background thread and we return the `WebView` backend so the main thread is free to render install progress.

<!--snippet file=src/android/proot/setup.rs fn=setup-->

Once a backend exists, Phase 2 takes over. winit calls `resumed` every time the activity becomes visible, and this is the runtime fork that mirrors the setup fork: the WebView backend shows an HTML progress page; the Wayland backend brings up the GPU renderer and launches the desktop.

<!--snippet file=src/android/app/run.rs fn=resumed-->

## The WebView path — first-run installer

This path runs once in a device's life: the first time the app opens, before the Linux environment exists. Almost all of the *work* lives here — downloading a root filesystem, unpacking it, running `pacman`, and writing dozens of config files — but none of it is the product. It exists only to get the device to the point where the Wayland path can take over.

The user sees a local HTML page (`setup-progress.html`) in an Android WebView. The `resumed` arm above points that WebView at the page and passes it a port; the page opens a WebSocket back to the app to receive live progress.

`setup` builds an ordered list of stages and walks it. Each stage is idempotent and returns `None` if its work was already done, or a `JoinHandle` if it spawned work. The orchestration loop runs the first unfinished stage, then chains the rest on a background thread, pushing a percentage to the progress channel as it goes — which is why the main thread stayed free to draw the page.

The stages, in order:

1. `setup_arch_fs` — download and extract the Arch Linux ARM64 root filesystem.
2. `simulate_linux_sysdata_stage` — fabricate the `/proc` and `/sys` files proot can't provide.
3. `install_dependencies` — `pacman -S` the desktop, retrying through a flaky network.
4. `setup_machine_id` — seed the *guest's* `/etc/machine-id` and `/var/lib/dbus/machine-id` for D-Bus clients inside proot.
5. `setup_pipewire_package_lock` — hold the guest PipeWire packages, since audio is served by the Android-side PipeWire (see below).
6. `setup_firefox_config` — pre-seed Firefox prefs so it runs unsandboxed under proot.
7. `setup_fake_bwrap` — replace Bubblewrap with a shim (Android has no user namespaces).
8. `setup_chromium_no_sandbox` — make Chromium/Electron apps launchable without a terminal.
9. `setup_onboard_signal_fix` — wrap the on-screen keyboard around a proot `fstat` quirk.
10. `setup_xfce_wayland` — write the Xfce/labwc session config and HiDPI scaling.
11. `fix_xkb_symlink` — repoint an absolute xkb symlink to a relative one (it's resolved from NDK, not the chroot root).

The first stage is the heaviest and the most illustrative — note that it is one big retry loop: a corrupt download deletes the archive and starts over rather than failing the install.

<!--snippet file=src/android/proot/setup.rs fn=setup_arch_fs-->

Installing the desktop is the other stage worth reading. `pacman` over a phone's network is unreliable, so `install_dependencies` clears any stale lock and retries up to ten times, streaming each line of pacman output to the progress page, and only gives up — loudly — after the last attempt.

<!--snippet file=src/android/proot/setup.rs fn=install_dependencies-->

Behind the progress page is `WebviewBackend::build`. It binds an ephemeral localhost port, then spins up two threads: one forwards `SetupMessage`s from the install pipeline to the connected browser, the other accepts the browser's WebSocket connection. The port it picked is the one handed to the WebView URL up in `resumed`.

<!--snippet file=src/android/backend/webview.rs fn=build-->

When the pipeline reaches 100%, it asks the user to restart. On the next launch every stage reports "already done", `setup` returns the `Wayland` backend, and the device never visits this path again.

## The Wayland path — the product

This is what Local Desktop *is*: a Wayland compositor running inside the Android NDK, with an Xfce desktop running as its client inside proot, rendered back into the native activity. The `Wayland` arm of `resumed` is its entry point. On every resume it (re)creates the GPU renderer, configures the output to match the Android window, marks the runtime active, draws one frame, and launches the desktop session.

The backend struct is the compositor's live state — the smithay `Compositor`, the winit GPU renderer (dropped on suspend, rebuilt on resume), the input clock, and the touch-gesture bookkeeping that turns finger gestures into pointer events.

<!--snippet file=src/android/backend/wayland/mod.rs struct=WaylandBackend-->

`bind` constructs the winit + smithay GLES renderer against the activity's surface. It returns a `Result` because GPU init can fail (notably on resume races), and `resumed` degrades gracefully when it does.

<!--snippet file=src/android/backend/wayland/winit_backend.rs fn=bind-->

`configure_output` reconciles the compositor with the physical Android window every time the surface changes: it creates or updates the smithay `Output` (mode and transform, with the parent output scale fixed at 1 because rendering uses physical pixels), writes the host geometry and guest UI scale to a file the in-chroot `wlr-randr` script watches, and hands the window size and density scale to `State::set_host_geometry`, which resizes every toplevel to fill the screen. Resize events (rotation, fold/unfold, moving to another display) take the same route from `handle`.

<!--snippet file=src/android/app/run.rs fn=configure_output-->

### Frame loop

A watcher thread wakes the event loop for client requests, and Android's `AChoreographer` vsync clock paces redraws to at most one per display refresh; the loop sleeps when nothing changes. Frame callbacks go out right after a commit when a refresh has passed since the last release, otherwise at the next vsync, so labwc drawing on the CPU keeps its own pace and KWin on the GPU runs at the display's rate. Design and the test that chose it: [frame-loop.md](frame-loop.md).

### Scaling — two kinds of client

The guest scale comes from the density of whichever display the activity is on (`ndk::scale_factor`, `densityDpi / 160`; 2.625 on a 420 dpi phone). Clients use it in one of two ways:

- **Pixel clients** (labwc, Xfce) are configured in window pixels and draw at 1:1. They scale their own UI from the rounded scale in `/tmp/localdesktop-output`.
- **Scale-aware clients** opt in by asking for `wp_fractional_scale_v1` on their surface. The compositor sends them the exact density scale as their preferred scale, configures them in logical units (window size ÷ scale), renders their surface tree scaled up by the same factor, and divides pointer positions and scroll distances by it. Nested KWin (the Plasma Mobile session) is the client this exists for: KWin 6.7's nested backend takes its output scale **only** from this protocol, and ignores `--scale` after the first frame, along with kscreen and `kwinoutputconfig.json`.

`surface_scale` is the single switch: it returns the preferred scale only for surfaces that opted in, so the pixel path is untouched.

<!--snippet file=src/android/backend/wayland/compositor.rs fn=set_host_geometry-->

On a change of window size or density, the preferred scale is sent *before* the configure, because KWin only applies a pending scale when the next configure arrives. Integer scales round-trip exactly. With fractional ones, the logical size is rounded, so the scaled surface can fall up to one pixel short of the window edge.

### Planned — multiple displays

Today one Android activity window is one compositor output, and every toplevel fills it. External displays work in the sense that moving the activity to one (DeX, or a USB-C monitor) is just another resize and density change. A Linux desktop *spanning* the phone and a monitor needs more:

1. **Android side:** one native window per display, the activity's own plus a `Presentation` (or a second activity) on each `DisplayManager` display, each with its own size and density, tracked through `DisplayManager.DisplayListener`.
2. **Compositor:** one host-display record per window (size, density scale, smithay `Output`), with each toplevel assigned to one record. `set_host_geometry` then becomes per display, rendering draws each window's own toplevels, and pointer targeting picks the toplevel on the touched window instead of `get_surface`'s first toplevel. The per-surface scale above already allows different scales per display.
3. **KWin:** the nested backend creates `--output-count` outputs at start, and adds or removes outputs at runtime through `createVirtualOutput` / `removeVirtualOutput`, each one a new or destroyed `xdg_toplevel` on our compositor. A display hot-plugged on Android has to reach that path, for example through a guest helper that requests a virtual output (`zkde_screencast_unstable_v1.stream_virtual_output`, as `krfb-virtualmonitor` does). The compositor then binds the new toplevel to the new display record. KWin's own output layout (kscreen) arranges the monitors.

With the compositor live, `launch` starts the desktop. It guards against double-launch with an atomic flag, clears stale X11 lock files, then runs `startxfce4 --wayland` as the configured user *inside proot* on a background thread. From here, Xfce is a normal Wayland client of our compositor.

<!--snippet file=src/android/proot/launch.rs fn=launch-->

The other half of the product is input. Android delivers events to winit; `window_event` is the per-frame entry. It funnels every raw event through `centralize` (which normalises winit/touch events into our own `CentralizedEvent`, including two-finger-scroll detection) and then `handle` (which feeds them into the smithay compositor and requests redraws). Physical-keyboard events injected by the accessibility service take a parallel route through `user_event` → `centralize_injected_keyboard`.

<!--snippet file=src/android/app/run.rs fn=window_event-->

Audio does not come from the guest. `pipewire_standalone_aaudio` runs PipeWire and WirePlumber as *Android-side* processes, outside proot, from libraries in the APK's `nativeLibraryDir`. It talks to the device through AAudio and publishes its sockets (including PipeWire-Pulse at `/tmp/pulse/native`) into the guest's `/tmp`, so guest clients find them through the `PIPEWIRE_RUNTIME_DIR` and `PULSE_SERVER` exported by the launch command. Every daemon's environment is assembled in one place:

<!--snippet file=src/android/backend/pipewire_standalone_aaudio.rs fn=apply_wireplumber_env-->

**Known issue — WirePlumber's D-Bus support (fix belongs in `apply_wireplumber_env`).** The bundled WirePlumber is a Termux-prefixed build. Because it runs outside the guest, the guest machine-id that `setup_machine_id` seeds never applies to it. With no `DBUS_SESSION_BUS_ADDRESS` in its environment, it tries to autolaunch a bus, looks for a machine-id under `/data/data/com.termux/files/usr/`, fails, and skips the optional `support.dbus` component and everything that depends on it (`support.portal-permissionstore`, `script.client.access-portal`, and device reservation). Playback is unaffected, but the logs fill with warnings, and portal-mediated access will not work. The stage list above is **not** the place to fix this. The fix belongs on the Android side, in this module:

- **Preferred:** have `prepare_wireplumber_share` write a `wireplumber.conf.d/` drop-in that disables `support.dbus` in the `main` profile. Nothing on the Android side needs a bus.
- **Alternative:** once the guest session starts a session bus at a known path (for example under the guest `/tmp`), export `DBUS_SESSION_BUS_ADDRESS=unix:path=<ARCH_FS_ROOT>/tmp/…` from `apply_wireplumber_env`. `resumed` spawns WirePlumber right after `launch()`, so the two race and there is no guarantee the guest bus exists yet. This option needs explicit ordering or a WirePlumber restart, and is only worth it if portal integration is wanted.

### Planned — two-way notification bridge

*Not implemented yet.* The goal is that a notification raised in the Linux desktop shows up in Android's notification shade, and an Android notification shows up in the Linux desktop. The bridge has to cross the same boundary as audio: freedesktop notifications live on the guest's D-Bus session bus *inside* proot, while Android notifications live in the app process *outside* it. It reuses two patterns that already exist:

- **The JNI service bridge from input.** `KeyboardAccessibilityService.java` declares `native` methods that land in `accessibility.rs`, which queues events and wakes the winit loop through an `AppUserEvent`. A notification listener service follows the same shape.
- **The guest `/tmp` rendezvous from audio.** The guest's `/tmp` is a plain directory under `ARCH_FS_ROOT`, so a Unix socket created there is reachable from both sides. That is how the PipeWire sockets cross today.

The pieces, and where each belongs:

1. **Wire protocol:** a Unix socket at `<ARCH_FS_ROOT>/tmp/localdesktop-notify.sock`, owned by the Android side, carrying newline-delimited JSON messages: `post`, `close`, `action`, each with an origin, an ID, an app name, a summary, a body, an icon, and an action list.
2. **Guest agent:** a new setup stage alongside `setup_chromium_no_sandbox`, plus an autostart entry, installs a small daemon in the guest. This keeps it out of the per-desktop launcher, so it works under both Xfce and Plasma. The daemon:
   - **Linux → Android:** becomes a D-Bus *monitor* (`org.freedesktop.DBus.Monitoring.BecomeMonitor`) for `org.freedesktop.Notifications.Notify` and `CloseNotification`, and for the server's replies, which carry the assigned ID. It forwards each one to the socket. Because it only monitors, the desktop's own notification server (xfce4-notifyd, or plasmashell) keeps working unchanged. Owning the name instead would conflict with Plasma.
   - **Android → Linux:** calls `Notify` on the session bus for each `post` received from the socket. It sets a hint `x-localdesktop-origin=android` so its own monitor ignores the echo. It maps `ActionInvoked` and `NotificationClosed` signals back to `action` and `close` messages.
   - **Bus address:** the agent needs a predictable session bus address. Today `dbus-run-session` picks a random one. The launch command (`default_launch` in `config.rs`) should start the bus at a fixed `unix:path=/tmp/dbus-session` and export `DBUS_SESSION_BUS_ADDRESS`. That is also the address the second option for the WirePlumber D-Bus issue above would use.
3. **Android side, Linux → Android:** a new `notifications.rs` beside `accessibility.rs`. It listens on the socket and posts through `NotificationManager` over JNI, on a "Linux desktop" channel. Each notification's content intent reopens the activity with the guest ID as an extra, and that comes back as a new `AppUserEvent` variant and is sent to the guest as `action` / `default`. Android 13+ (`target_sdk_version: 33`) also needs a runtime request for `POST_NOTIFICATIONS`. The permission is declared in `manifest.yaml`, but nothing requests it yet.
4. **Android side, Android → Linux:** a new `NotificationBridgeService.java` extending `NotificationListenerService`, declared in `manifest.yaml` next to the accessibility service with `android.permission.BIND_NOTIFICATION_LISTENER_SERVICE`. The user grants it in *Settings → Notification access*, the same flow as the accessibility service. `onNotificationPosted` and `onNotificationRemoved` call `native` methods that forward to the socket. The service **must skip notifications whose package is `app.polarbear`**, or the two directions feed each other in a loop. When the user dismisses a mirrored notification in the guest, the service calls `cancelNotification(key)`.

Constraints to design around:

- The guest only exists while the activity's session is running. Android-side posts that arrive while it is down should be dropped, or the last few queued, as `accessibility.rs` does with `pending_events`. They must never block the listener service.
- Linux notification icons arrive as icon names or as `image-data`, while Android wants a `Bitmap`. Small icons must come from the APK, so every mirrored notification uses the app's own small icon, with the guest's icon as the large icon.
- Pass through only notification contents, never arbitrary commands. The socket is world-reachable inside the guest, so treat every message as untrusted input.

That is the whole runtime loop: resume → bind renderer → configure output → launch desktop → translate input → render, suspend tears the renderer down, and the next resume rebuilds it. Everything below this line is about producing the `.apk` that ships all of the above.

# `xbuild` — the cross-compiling build path

The shipped APK is built with [`xbuild`](https://github.com/rust-mobile/xbuild), vendored and patched under `patches/xbuild/` (the patches add Android-*host* support — building on-device in Termux — plus AGP/KGP compatibility). This is the path the desktop pipeline and `scripts/build-termux.sh` use; the canonical invocation is one line:

<!--snippet file=src/bin/build_apk.rs lines=1-1-->

(For reference, that single command is `x build --release --platform android --arch arm64 --format apk`.)

The CLI is a `clap` subcommand enum. `Build` flattens straight into `BuildEnv::new` followed by `command::build`; `Run`, `Test`, and `Lldb` are the same build step plus a device action, which is why they all share `BuildArgs`.

<!--snippet file=patches/xbuild/xbuild/src/main.rs fn=run-->

`command::build` is the engine. It runs as three tasks: fetch any precompiled artifacts, build the Rust crate for each target arch (note `--lib` — for Android we build the `cdylib`, not a binary), then package the result into the requested format. For our platform that final step constructs an `Apk` from the freshly built `lib*.so`. The head of the function shows the shape:

<!--snippet file=patches/xbuild/xbuild/src/command/build.rs fn=build-->

The packaging `xbuild` does here is the same job the next chapter does standalone — which is the point of having both.

# `build_apk` — the no-cross-compile build path

On an arm64 device there is nothing to cross-compile, so the project ships its own APK builder and makes it the default binary (`default-run = "build_apk"` in `Cargo.toml`). A bare `cargo run` on-device builds, packages, and signs the APK with no `xbuild`, no Gradle, and no Android Studio — it is a self-contained reimplementation of the packaging half of the previous chapter.

The whole thing is `apk::build`. It parses `manifest.yaml`, synthesises a complete `AndroidManifest` (filling in sane defaults for SDK levels, the launcher activity, the `android.app.lib_name` meta-data that points Android at our `cdylib`, and the `MAIN`/`LAUNCHER` intent filter), runs `cargo build --lib` to produce the `.so`, then assembles the APK: resources and icon, declared assets, the native library for the host arch, and the prebuilt `classes.dex` — finishing with a signing pass.

<!--snippet file=src/bin/build_apk.rs fn=build-->

The packaging primitives it leans on are small and worth knowing by name:

- `Apk::add_res` compiles `resources.arsc`, scales the icon to every density, and serialises the binary `AndroidManifest.xml`.
- `Apk::add_lib` drops the `cdylib` into `lib/<abi>/`.
- `Apk::finish` closes the zip and hands it to the RSA/SHA-256 `Signer`.

`add_res` is the most involved — it is where the human-readable `manifest.yaml` becomes the binary resource table Android actually reads:

<!--snippet file=src/bin/build_apk.rs fn=add_res-->

Both build paths converge on the same artifact — a signed APK whose `cdylib` exports the `android_main` we started this document with. The call stack closes on itself.
