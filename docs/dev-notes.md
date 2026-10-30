# Local Desktop: development notes

Repo: https://github.com/localdesktop/localdesktop.github.io (cloned at `~/localdesktop`, commit `e6a8b58`, 2026-09-08)

## Done (2026-10-01)

### 1. Built the APK with Docker (`make build-docker`, as the README describes)
- Host: Fedora 44 KDE. Docker images live in `/var/lib/docker` on `/`, which has plenty of space. `/home` is tight, about 9 GB free.
- **The Dockerfile needed one fix.** The base image `alvrme/alpine-android:android-30-jdk17` was rebuilt on 2026-07-14 on BellSoft Alpaquita (glibc), not Alpine, so it has no `musl-dev` package. I removed `musl-dev` from the `apk add` line. This change is uncommitted; `git diff` shows it and `git checkout Dockerfile` reverts it. It may be worth reporting upstream.
- Outputs in `~/localdesktop/target/x/release/android/`:
  - `gradle/app/build/outputs/apk/debug/app-debug.apk` (8.9 MB, debug-signed). **This is the one to install.**
  - `localdesktop.apk` (7.6 MB). This is the file the README names, but it's **unsigned**, so Android won't install it.
- The base image is still in Docker: `docker rmi alvrme/alpine-android:android-30-jdk17` frees the space.

### 2. Installed it on the phone
- Test phone: a foldable (main and cover displays), arm64-v8a, adb serial `<adb-serial>`.
- `android-tools` (adb) is installed on the PC. USB debugging is on and this PC is authorized.
- Installed with `adb install app-debug.apk`. Package ID **`app.polarbear`** (the in-app name is Local Desktop), version 2.1.1.
- A spare copy is at phone `Internal storage/Download/localdesktop-debug.apk`. It can be deleted.

### 3. First-run setup was in progress
- I last checked at about 17:20. pacman was installing the Arch ARM base and XFCE, with no errors (only the usual "dependency cycle" warnings).
- After that the phone went onto a wall charger (unplugged from the PC) and stayed on it until it reached at least 20% battery, with the app kept in the foreground.
- **Not yet confirmed:** whether setup finished and the XFCE desktop appeared.

## Next session: plan

0. **Check setup finished.** Plug the phone in, run `adb devices`, then `adb logcat -d --pid=$(adb shell pidof app.polarbear)`, or just look at the screen.
1. **Switch the desktop to KDE Plasma Mobile.**
   - Local Desktop starts an XFCE *Wayland* session inside proot, using its own small compositor in the app. Plasma normally needs its own compositor (KWin), so Plasma Mobile may not run as a drop-in replacement. Find out how the app picks the session to launch (`src/`, `docs/architecture.md`, the developer manual) before installing anything.
   - Plasma Mobile packages on Arch Linux ARM: check whether `plasma-mobile` is available in the repos (or the AUR).
   - A fallback, if Plasma Mobile won't run under the app's compositor: plain Plasma desktop pieces, or keep XFCE and tune it for touch.
2. **Browser: Brave Origin Nightly** (the user's choice).
   - Brave publishes official Linux **arm64** builds of Origin Nightly. On Arch, use the AUR package `brave-origin-nightly-bin` (`yay -S brave-origin-nightly-bin`). Brave's other route is `curl -fsS https://dl.brave.com/install.sh | FLAVOR=origin CHANNEL=nightly sh`, but that targets deb/rpm distros.
   - Things to check: install `yay` and `base-devel` inside the chroot first, and confirm the AUR PKGBUILD lists `aarch64` (if it doesn't, its arch field may need editing). Chromium-based browsers usually need `--no-sandbox` under proot, because proot can't provide user namespaces or seccomp. That flag can go in a `.desktop` file or in `~/.config/brave-origin-nightly-flags.conf`, if the package reads one.
3. **Steam: official ARM64 client plus FEX** (the user's lead was right).
   - Valve has shipped an official ARM64 Linux Steam client for months. Proton 11 (beta) bundles **FEX**, which runs x86 Windows games on ARM64 out of the box. This is the stack the Steam Frame uses (Snapdragon 8 Gen 3, launched 2026-09-18). It's been shown working on Raspberry Pi, Asahi Macs and Switch.
   - Some games now ship native ARM64 Linux builds (for example Factorio, 2026-09), which need no emulation at all.
   - **Open questions specific to Local Desktop:**
     - *proot:* Can FEX run without binfmt_misc or real root? Proton launches it directly, which may help. Test with a small game first.
     - *GPU:* The test phone is a Snapdragon 8 Gen 2 with an Adreno 740 GPU. Inside proot, Mesa may fall back to software rendering (llvmpipe), which would make 3D games very slow. Check `glxinfo -B` or `vulkaninfo`, and whether Turnip/KGSL (Mesa's Adreno driver) can reach the GPU from the app.
     - *Storage and RAM:* games plus the FEX rootfs take a lot of space, so check free internal storage.
   - Expectations: native ARM64 games and light x86 games are realistic. Heavy 3D games depend on the GPU question.

## Useful commands
```bash
adb devices -l
adb install -r ~/localdesktop/target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk   # reinstall/upgrade
adb logcat -d --pid=$(adb shell pidof app.polarbear) | tail -50
cd ~/localdesktop && make build-docker    # rebuild (keep the Dockerfile fix)
```

## Done (2026-10-02): Phase 0 of the Plasma Mobile plan

- **Setup finished.** All 19 packages the app checks for are installed, and the rootfs is 3.0 GB. XFCE comes up (when folded it appears on the cover screen, display id 4630946481096930692).
- **Two displays:** screenshots need `adb exec-out screencap -d <id> -p`. List the IDs with `adb shell dumpsys SurfaceFlinger --display-id`.
- **Helper `~/bin/ld-exec '<cmd>'`** runs a command as root inside the Arch proot over adb, using `run-as` and the app's own `libproot.so` with the same binds the app uses. Ignore the `proot warning` lines it prints. The app does not have to be running.
- **Session config:** `/etc/localdesktop/localdesktop.toml` does not exist yet, so the defaults are in use. Create it to override `check`, `install` and `launch`. A `try_launch` line is used once, then commented out.
- **Packages (ALARM, aarch64):** available are plasma-mobile, plasma-workspace, kwin and plasma-nano at 6.7.5, plus plasma-keyboard 6.7.5 (KDE's newer on-screen keyboard, which replaces Maliit), breeze, breeze-gtk, breeze-icons, kde-gtk-config, qt6ct, angelfish, kasts, koko, index-fm, discover, kclock, kweather, plasma-settings, qmlkonsole, mesa 26.2.3, vulkan-freedreno, mesa-utils and vulkan-tools.
  - **Not available:** maliit-keyboard, maliit-framework, plasma-dialer and spacebar. Dialer and Spacebar would be useless without a modem anyway.
- **GPU:** `/dev/kgsl-3d0` is crw-rw-rw- and readable from inside the app. **The Adreno GPU may be reachable via Turnip's KGSL backend.** Still to check: whether ALARM's Mesa is built with `freedreno-kmds=kgsl`. `/dev/dri` is not accessible.
- **Correction: the guest machine-id is fine.** `setup_machine_id` seeds it, and I misread a garbled `cat` over adb. The WirePlumber D-Bus warning comes from the **Android-side** WirePlumber (a Termux-prefixed build running outside proot). The fix belongs in `src/android/backend/pipewire_standalone_aaudio.rs` (`prepare_wireplumber_share` / `apply_wireplumber_env`). This is now documented in `docs/architecture.md` (uncommitted). It isn't needed before Plasma: the guest session starts its own bus with `dbus-run-session`.
- **Backup:** phone `files/arch-backup-2026-10-02.tar` (2.9 GB, made with the app stopped). It skips sockets and `/usr/lib/dbus-daemon-launch-helper`, which `pacman -S dbus` restores. To restore: `adb shell am force-stop app.polarbear; adb shell run-as app.polarbear sh -c 'cd files && rm -rf arch && tar -xf arch-backup-2026-10-02.tar'`.
- **Next:** Phase 1, the look without changing the session: Breeze themes, postmarketOS wallpapers, Kirigami apps, plasma-keyboard.

## Planned feature: two-way notification bridge (Linux ↔ Android)
The user wants guest notifications sent to Android's shade, and Android notifications shown in the Linux desktop. The design is in `docs/architecture.md`, under "Planned — two-way notification bridge" (uncommitted). In brief:
- A socket in the guest's `/tmp`.
- A guest agent that watches D-Bus (as a monitor) for Linux → Android, and calls `Notify` for Android → Linux.
- A Rust `notifications.rs` that uses NotificationManager over JNI.
- A Java `NotificationListenerService`, modelled on `KeyboardAccessibilityService`.
- The session bus needs a fixed address.
- A runtime request for `POST_NOTIFICATIONS`.
- Loop prevention: skip `app.polarbear`'s own notifications, and tag guest posts with an `x-localdesktop-origin` hint.

## Phase 1 (2026-10-02): KDE / postmarketOS look on the XFCE session
- **`ld-exec` has NO network.** `run-as` gives the app's UID but not Android's `inet` group. The app itself does have network.
  - Install packages with **`~/bin/ld-pacman pkg...`** instead. It resolves URLs inside the guest (against the current sync DBs), downloads the packages and `.sig` files on the PC (the guest uses `SigLevel = Required`), pushes them into the pacman cache, and installs offline.
  - Don't run `pacman -Sy` via `ld-exec`; it fails silently. To refresh the DBs, run `pacman -Syu` in the app's own terminal.
  - The earlier "missing machine-id" and garbled `cat` output came from stdout handling under `run-as`. For files, use `while read` or `adb shell run-as ... cat`.
- **Installed:**
  - Theming: breeze, breeze-gtk, breeze-icons, kde-gtk-config, qt6ct, qt6-wayland, noto-fonts-emoji, plasma-integration, qqc2-breeze-style. plasma-integration pulled in kwin, plasma-workspace and xdg-desktop-portal-kde, which Phase 2 needs.
  - Apps and keyboard: plasma-keyboard, angelfish, kasts, koko, index-fm, kclock, kweather, plasma-settings, qmlkonsole. About 245 packages in total.
  - `accounts-daemon-restart.hook` fails because there's no systemd. That's harmless.
- **postmarketOS wallpapers:** 26 KDE-format packages from gitlab.postmarketos.org/postmarketOS/artwork, installed as `/usr/share/wallpapers/postmarketOS-<year>-<name>` (CC BY-SA 3.0; licence in `/usr/share/licenses/postmarketos-artwork/`). The default is `postmarketOS-2026-basin`.
  - Warning: wiki.postmarketos.org now 301-redirects to an unrelated domain (wiki.nura.eco). I did not follow it.
- **Config (in files the app does NOT rewrite):**
  - `/etc/localdesktop/localdesktop.toml`: `[command] launch` is the default XFCE launch plus `QT_QPA_PLATFORMTHEME=kde`, `QT_QUICK_CONTROLS_STYLE=org.kde.breeze`, `QT_QUICK_CONTROLS_MOBILE=1`, `QTWEBENGINE_DISABLE_SANDBOX=1` and Breeze cursors. Delete the file to revert. The app's log confirmed it picked this up.
  - `/usr/local/bin/localdesktop-kde-look`, run by `~/.config/autostart/localdesktop-kde-look.desktop`: sets the GTK, icon, cursor and font settings over xfconf, and the wallpaper per wlr output. To use a different wallpaper, set `WALLPAPER=/path` or edit the default in the script.
  - `/root/.config/kdeglobals` (BreezeLight, breeze icons, Noto Sans, Hack) and `/root/.config/gtk-3.0/settings.ini`.
  - The app rewrites these on every launch: `xsettings.xml`, `~/.config/xfce4/labwc/{environment,rc.xml,autostart}`, and `/usr/local/bin/startxfce4-localdesktop`. Never edit those.
- **Keyboard:** Onboard stays under XFCE. plasma-keyboard needs KWin's input-method support, so it only works in Phase 2.
- **Not verified yet:** what it looks like on screen. The phone locked before the app reached the foreground, so the session didn't start.
- **Verified on screen (2026-10-02, about 12:20 CDT):** the basin wallpaper, Breeze icons and Noto fonts are all in place. KWeather renders with Breeze/Kirigami styling.
- **Found and fixed:**
  - **Qt Quick apps crashed with SIGILL (exit 132)** in Mesa **llvmpipe**. No GPU driver is found (`failed to get driver name for fd -1`), so Mesa falls back to llvmpipe, whose JIT hits an illegal instruction under proot. Fix: added `QT_QUICK_BACKEND=software` to the toml launch line. The previous version is saved as `localdesktop.toml.bak`. GTK4 and GL apps may still hit this until Turnip/KGSL works.
  - **Timezone:** `/etc/localtime` was missing, so the clock showed UTC. It now links to America/Chicago (and `/etc/timezone` is set).
- **Testing gotchas:**
  - `wayland-0` is the **app's** compositor. labwc (where XFCE apps live) is `wayland-1`, and X is `DISPLAY=:1`.
  - An `ld-exec` proot can't authenticate to the session's D-Bus (`dbus-send` gets no reply). Apps that need D-Bus, such as KClock (which talks to `kclockd`), exit 1 when started from `ld-exec`. Test those from the session's own menu instead.
  - `ld-exec` uses `--kill-on-exit`, so anything backgrounded dies when the command returns. Run a test in the foreground with `timeout N` and take the screenshot from a parallel adb call.
- **KClock (12:29):** the user's first tap seemed to do nothing. On the second tap, made through a logging wrapper (since removed), it opened fine. The log showed only harmless warnings: xx-pip-v1 is unsupported, and portal registration failed with `Unable to open /proc/<pid>/root`, which is a proot limit. The likely cause of the first failure is a slow first start while QML compiles its cache with software rendering. I couldn't confirm that.
  - Tip: Qt sends its logs to journald unless `QT_FORCE_STDERR_LOGGING=1` is set. Without it, errors simply vanish.
- **Cosmetic:** the bottom XFCE dock (panel 2) sits on top of windows and covers Kirigami's bottom tab bar. Fix it by having the dock reserve its space or autohide.

## Dock fix and Breeze Dark (2026-10-02, about 12:33)
- **Dock:** panel-2's `autohide-behavior=1` ("intelligent") needs window geometry, which labwc doesn't expose. So it never hid and reserved no space. `localdesktop-kde-look` now sets it to 0, so the dock is always shown and windows stop above it.
- **Dark theme:**
  - xsettings: Breeze-Dark and breeze-dark icons, set in `localdesktop-kde-look`.
  - gsettings: `color-scheme=prefer-dark`, also in `localdesktop-kde-look`.
  - `~/.config/gtk-{3,4}.0/settings.ini`: Breeze-Dark with `prefer-dark`.
  - `kdeglobals`: BreezeDark `[Colors:*]` groups merged in by hand, because `plasma-apply-colorscheme` aborts without a display. The light version is saved as `kdeglobals.light.bak`.
  - `~/.config/xfce4/labwc/themerc-override`: Breeze Dark title bars and menus. The app does not rewrite this file.
- Verified with KWeather: dark title bar and dark Kirigami controls.

## Phase 2 (2026-10-02, about 13:03): Plasma Mobile session WORKING (launched once via try_launch)
- **Installed:** plasma-mobile, plasma-nano, pipewire-pulse (at the pinned 1:1.6.9; listed explicitly because the app's `IgnorePkg` pins pipewire), base-devel and gdb. gdb is useless under proot.
- **Session chain:** app compositor (wayland-0) → bare **labwc** (config `/root/.config/labwc-plasma`, not app-managed) → **KWin** (`--fullscreen --xwayland`, socket wayland-2, `--inputmethod plasma-keyboard`) → `/usr/local/bin/localdesktop-plasma-mobile-shell` (`dbus-update-activation-environment`, envmanager, kded6, `plasmashell -p org.kde.plasma.mobileshell`).
  - Launcher: `/usr/local/bin/startplasmamobile-localdesktop`. Log: `/tmp/plasma-mobile-session.log`.
  - **Why labwc is in the middle:** KWin 6.7's nested backend hard-requires `wp_single_pixel_buffer_manager_v1`, `wp_viewporter`, `zwp_pointer_constraints_v1` and `wp_presentation_time`. The app's smithay compositor lacks all four, and KWin exits 1 with only a warning. **Proper fix:** add these protocols in the app's Rust compositor (smithay has delegates for each), rebuild, then drop labwc.
  - **Why not startplasmamobile:** startplasma waits on systemd and on xdg-desktop-portal(-kde/-gtk) D-Bus activation, which time out at 120 s each under proot.
- **Fixes/workarounds:**
  - **udev netlink shim** at `/usr/local/lib/udev-netlink-shim.so` (source: `/usr/local/src/udev-netlink-shim.c`, loaded with LD_PRELOAD). Android SELinux denies `NETLINK_KOBJECT_UEVENT` sockets (EACCES), libudev's monitor is then NULL, and KWin 6.7 `GpuManager` calls `m_udevMonitor->fd()` on it and segfaults. That's arguably a KWin bug worth reporting upstream: there's no null check in `gpumanager.cpp:79`. The shim hands out a dummy AF_UNIX socket instead.
  - `KWIN_COMPOSE=Q` gives QPainter software compositing.
  - `startkderc [General] systemdBoot=false` is set, though it's unused now.
- **Debug tools:**
  - `/usr/local/lib/segvbt.so` is an LD_PRELOAD backtrace-on-crash handler, needed because gdb and strace can't attach under proot.
  - **Currently enabled in the launcher**, together with `QT_LOGGING_RULES=kwin_*.debug`. The clean version is at `/tmp/startplasmamobile-localdesktop.good`.
  - The first app-launched attempt died silently right after KWin started, while the second (identical apart from debug) worked. Keep the debug on until it's reliable.
- **try_launch is single-use.** The next app start goes back to XFCE until `launch` itself points at the new launcher.
- To revert to XFCE, do nothing (just restart the app). The XFCE `launch` line is untouched.
- **Keyboard (13:10):** plasma-keyboard registers as KWin's input panel, but KWin's default `VirtualKeyboardMode` (NonMouseInput) only shows it after touch or tablet input. Taps likely arrive through the app → labwc chain as pointer events. Set `kwinrc [Wayland] VirtualKeyboardMode=2` (AnyInput). Waiting on the user's test.
- **Keyboard confirmed working (13:12)** with `VirtualKeyboardMode=2`.
- **Plasma Mobile is now the PERMANENT session (13:13):**
  - `launch = "/usr/local/bin/startplasmamobile-localdesktop"`. The Xfce launch line is kept, commented out, directly below it. To switch back, swap the `# ` between the two lines.
  - The launcher falls back to Xfce if the Plasma session ends within 30 s. KWin's startup command is `sh -c '...; labwc --exit'`, so labwc no longer outlives a dead KWin.
  - KWin debug logging is off. `segvbt.so` stays preloaded to give backtraces on crashes.
- **Next options:**
  - Add the 4 missing protocols to the app's smithay compositor, which lets labwc be dropped (less lag).
  - GPU via Turnip/KGSL.
  - Brave Origin Nightly (base-devel is installed now).
  - Steam with FEX.

## APK rebuild + GPU (2026-10-02, about 13:20–13:45): PAUSED. Resume here.
### Current state (everything works; nothing was lost)
- **Phone runs the OLD APK** (liblocaldesktop.so md5 `54f3fd…`). Plasma Mobile runs through the **labwc fallback** path.
- **Launcher** `/usr/local/bin/startplasmamobile-localdesktop` tries three paths, falling through when a session dies within 30 s:
  1. Direct KWin on wayland-0 with `--scale $LOCALDESKTOP_OUTPUT_SCALE` (read from `/tmp/localdesktop-output`; currently 3).
  2. labwc bridge.
  3. Xfce.
  - On the old APK, path 1 exits at once (`wp_single_pixel_buffer_manager_v1 isn't supported`), so path 2 runs. This is expected.
  - KWin args now use `--fullscreen=true`. The old `--fullscreen --xwayland` swallowed `--xwayland` as the value of `--fullscreen`.

### New APK (built, NOT installed)
- **Source changes** (uncommitted, in `~/localdesktop`), adding the 4 protocols KWin 6.7 needs:
  - `src/android/backend/wayland/compositor.rs`: ViewporterState, SinglePixelBufferState, PointerConstraintsState (no-op handler) and PresentationState (a `Clock<Monotonic>`), plus delegates and a `take_presentation_feedback_surface_tree()` helper.
  - `event_handler.rs` `redraw()`: collects wp_presentation feedback while drawing and sends `presented(Vsync, refresh from output mode, frame_seq)` after `submit()`, or `discarded` when there's no output.
- **Built OK.** It's at `~/localdesktop/target/target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk` (13:22, md5 of liblocaldesktop.so `288252…`). I verified the library contains the protocol strings.
  - **Gotcha:** `make build-docker` does `docker cp container:/app/target ./target`. When `./target` already exists, the output lands in **`target/target/`**. Either move or delete the old `target/` before building, or look in `target/target/`.
- **Install FAILS:** `INSTALL_FAILED_UPDATE_INCOMPATIBLE` (signature mismatch).
  - Every Docker build makes a fresh Gradle debug keystore inside the container, and `make build-docker` deletes the image, so the original key is gone for good.
  - Uninstall + install **wipes app data** (about 10 GB: `files/arch` is 6.9 GB, plus the 2.7 GB `arch-backup-2026-10-02.tar` and 613 MB of Mesa source in `/root/src`).
  - The first `adb install` "succeeded" only because I mistakenly installed the old APK from `target/x/...`.

### NEXT SESSION: option 1, chosen by the user
1. **Persistent signing key, so this never recurs.**
   - Generate a debug keystore on the PC: `keytool -genkeypair -keystore ~/localdesktop/.signing/debug.keystore -storepass android -keypass android -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000`.
   - Add `COPY .signing/debug.keystore /root/.android/debug.keystore` to the Dockerfile before the `x build` step, and gitignore `.signing/`.
   - Rebuild (`rm -rf target` first, or mind `target/target`).
2. **Back up the app data on the phone.** The phone has 785 GB free on /sdcard, and the stream stays on-device:
   - `adb shell am force-stop app.polarbear`
   - `adb shell "run-as app.polarbear tar -cf - files | cat > /sdcard/Download/localdesktop-files-backup.tar"`
   - Verify the size (about 10 GB). Optionally delete `files/arch-backup-2026-10-02.tar` first to save 2.7 GB of copying.
3. `adb uninstall app.polarbear`, then `adb install <new apk>`. **Don't launch the app yet.**
4. **Restore before the first launch:**
   - `adb shell "cat /sdcard/Download/localdesktop-files-backup.tar | run-as app.polarbear tar -xf - "`, run from the app data dir. `run-as` starts in `/data/user/0/app.polarbear`, so the archive's `files/...` paths land correctly.
   - Check that `files/arch/etc/localdesktop/localdesktop.toml` exists.
   - Then launch the app. Expect the log to show "direct KWin on wayland-0, scale 3" and stay there, with no labwc process.
   - Watch the scale (Plasma UI size) and the touch-coordinate mapping with `--scale 3`.
   - The new app UID gets new SELinux categories; files extracted by run-as should get the right labels, but check.
5. Keep the /sdcard backup until confirmed.
6. Afterwards: the GPU (below).

### GPU: Turnip with KGSL (built, NOT installed)
- ALARM's `vulkan-freedreno` (now installed) **lacks the KGSL backend**: no `/dev/kgsl-3d0` string in it, so it's msm-only. That backend is how Turnip reaches the Adreno through Android's kernel interface. `/dev/kgsl-3d0` is crw-rw-rw- and reachable from the app.
- Built Mesa 26.2.3 Turnip on the phone in the proot (about 10 min, `ninja -j8`):
  - Source: `/root/src/mesa-26.2.3`, build dir `build-kgsl`.
  - meson flags: `--prefix=/opt/turnip-kgsl -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm -Dgallium-drivers= -Dplatforms=wayland,x11`; no GL, EGL, GBM or LLVM.
  - The build **succeeded** (`build-kgsl/src/freedreno/vulkan/libvulkan_freedreno.so`). Not installed yet: run `ninja -C build-kgsl install` to put it in `/opt/turnip-kgsl`.
- **Next GPU steps:**
  1. Install the build.
  2. Test with `VK_ICD_FILENAMES=/opt/turnip-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json vulkaninfo --summary` (vulkan-tools and mesa-utils are installed). Expect "Turnip Adreno 740".
  3. If it works, consider a Zink-on-Turnip GL build in the same prefix (gallium-drivers=zink, with EGL and GBM enabled) for Qt and KWin GL. That removes `QT_QUICK_BACKEND=software` and the llvmpipe SIGILL issue.
  - Frames still reach the screen through wl_shm copies; dmabuf/AHardwareBuffer import in the app compositor is a later and bigger change.
- Also installed in the guest this session: vulkan-freedreno, vulkan-tools, mesa-utils, meson, ninja, glslang, spirv-tools, and other Mesa build deps.

## 2026-10-12: persistent signing key + new APK installed (data restored)
- **Persistent debug keystore:** `~/localdesktop/.signing/debug.keystore` (alias androiddebugkey, password android, SHA-256 `69:09:5F:E2:…:4F:31`). The Dockerfile copies it to `/root/.android/debug.keystore` before `x build`. `.signing/` is gitignored. **Back this file up**: losing it means another uninstall and restore.
- Rebuilt with `rm -rf target && make build-docker` (about 25 min; most of it is Docker exporting and unpacking the image). APK: `target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk`. Its liblocaldesktop.so contains all 4 new protocols.
- **Backup:** `/sdcard/Download/localdesktop-files-backup.tar` (6.6 GB, made from `files/`), plus `/sdcard/Download/dbus-daemon-launch-helper`, saved separately because its mode is 0110, so tar can't read it. Also on the phone: `ld-list.txt` (the tar listing) and `localdesktop-backup-errors.txt` (empty). The old `files/arch-backup-2026-10-02.tar` was deleted.
- Uninstalled, installed the new APK (new UID **u0_a84**, previously u0_a71) and restored with `cat tar | run-as app.polarbear tar -xpf -`. SELinux labels are fine (`app_data_file:s0:c84,...`).
  - **Restore gotcha:** toybox tar sets a read-only directory's mode *before* extracting its contents. `etc/ca-certificates/extracted/cadir` (0555) therefore came back empty, missing 438 entries. I regenerated it with `update-ca-trust` (363 certificates from the newer Mozilla bundle).
  - **Latent bug, worth fixing in the app's proot:** `update-ca-trust` fails (rc=1) because p11-kit parses `/usr/share/p11-kit/modules/.proot-meta-file.p11-kit-trust.module.meta` as a module config. The app's fake_id0 proot leaves its `.proot-meta-file.*` files visible inside the guest. This has been broken since setup (meta dated 2026-10-01 17:24), so ca-certificates upgrades would also fail. Workaround: from outside proot, move that meta file aside with run-as, run `update-ca-trust` in ld-exec, then move it back.
- `permission_all_files_access: false` after the reinstall. I'm not sure it was ever granted; check if anything needs /sdcard.
- **Still to verify:** the first launch of the new APK on screen. The app was launched while the phone was dozing, so the session didn't start. Expect "direct KWin on wayland-0" in `/tmp/plasma-mobile-session.log` and no labwc.

### 2026-10-12 (afternoon): direct KWin path working; scaling via wp_fractional_scale
- **The new APK works:** KWin runs directly on the app compositor (log line "direct KWin on wayland-0"), with no labwc.
- **First-touch crash (fixed in the guest, no APK change):** KWin segfaulted in `Pointer::motion → BackendOutput::position` (null output). Cause: `dbus-run-session` gave dbus-daemon `WAYLAND_DISPLAY=wayland-0`, so D-Bus-activated portals (GTK and KDE) connected to the **app** compositor. The launcher now runs `dbus-update-activation-environment WAYLAND_DISPLAY=wayland-2` inside the D-Bus session before KWin, on both the direct and labwc paths. Backups: guest `/tmp/startplasmamobile-localdesktop.pre-1012`, `.pre-scale`, `/tmp/localdesktop-plasma-mobile-shell.pre-1012`.
- **The UI was tiny (scale 1):** KWin 6.7's nested backend takes its output scale **only** from the host's `wp_fractional_scale_v1` (`wayland_output.cpp`: `applyChanges` ignores scale, and `applyConfigure` uses `m_pendingScale`). `--scale` only covers the first frame. kscreen-doctor and `kwinoutputconfig.json` have no effect.
- **The fix is in the app (uncommitted):** `wp_fractional_scale_v1` in `compositor.rs`.
  - Surfaces that opt in (KWin) get the exact Android density scale (420 dpi → 2.625). They're configured in logical size, rendered scaled, and receive pointer and scroll input divided by the scale.
  - labwc and Xfce never opt in, so they're unchanged.
  - The single entry point is `State::set_host_geometry(size, scale)`, called from `configure_output` (run.rs) and from the `Resized` event (rotation, fold/unfold, display change).
  - Expected logical sizes: inner screen 690×829 portrait and 829×690 landscape; cover screen 344×882.
- **Multi-monitor plan** is in `docs/architecture.md` under "Planned — multiple displays". In brief: one Android window per display, per-display records in the compositor, and KWin's nested `createVirtualOutput` adds outputs at runtime as new toplevels.
- **Android "16 KB page size" dialog:** the debug APK's libraries aren't 16 KB aligned. It's harmless on the test phone (4 KB pages). Fix later with `-Wl,-z,max-page-size=16384`.
- **Gotcha:** the Docker export and unpack takes about 15–20 min of the build. Compiling takes only about 2 min.
- **Rendering fix (13:35):** the first scaling build drew KWin correctly laid out but shrunk into the top-left 690×829 px. The scale argument of smithay's `render_elements_from_surface_tree` only positions subsurfaces; element *sizes* come from the draw scale (1.0). Each element is now wrapped in `RescaleRenderElement` with the surface's scale.
  - A protocol trace confirmed KWin handles fold changes live: it got 344×850 on the cover screen and 690×829 on the inner screen, both at scale 315/120 = 2.625.
  - KWin's nested window: the main surface is a single-pixel black buffer with viewport; the content is one subsurface (buffer 1811×2176, viewport → 690×829).

## PAUSED 2026-10-12 ~13:55. Resume here.
### Current state (everything installed and working)
- **The phone runs the latest APK** (built 13:47, fractional scale plus the RescaleRenderElement fix), signed with the persistent key. **Future builds install with `adb install -r`, with no data loss.**
- **Plasma Mobile runs DIRECT:** app compositor → KWin (wayland-2) → plasmashell. No labwc. Scale 2.625 (Android 420 dpi). Verified by screenshot: it fills 1812×2176 and looks sharp at a normal size.
- **Guest launcher** `/usr/local/bin/startplasmamobile-localdesktop`: the debug logging is removed, and the D-Bus env fix is on both the direct and labwc paths. The fallback chain (direct → labwc → Xfce) is unchanged.
- **Uncommitted source changes in `~/localdesktop`:**
  - Dockerfile: musl-dev removal; keystore COPY.
  - `.gitignore`: `.signing/`.
  - `compositor.rs`: the 4 protocols, plus fractional scale and `set_host_geometry` / `surface_scale`.
  - `event_handler.rs`: presentation feedback, per-surface pointer/scroll conversion, RescaleRenderElement.
  - `run.rs`: `configure_output` uses `set_host_geometry`.
  - `docs/architecture.md`: the WirePlumber issue, the notification bridge plan, the Scaling section, the multi-display plan.
  - Nothing committed or pushed yet. Ask the user before committing.
- **Backups still on the phone** (delete once the user confirms everything works): `/sdcard/Download/localdesktop-files-backup.tar` (6.6 GB), `dbus-daemon-launch-helper`, `ld-list.txt`, `localdesktop-backup-errors.txt`.
- **Old guest script backups** in the guest `/tmp`: `startplasmamobile-localdesktop.{pre-1012,debug-1012,pre-scale,nodebug}`, `localdesktop-plasma-mobile-shell.pre-1012`. They can be deleted.

### NEXT SESSION
1. **User testing still pending:**
   - Do taps land under the finger?
   - Does scrolling track the finger?
   - Does plasma-keyboard show at a normal size?
   - Does rotation to landscape work (expected logical size 829×690)?
   - Folding to the cover screen and back (344×882 on the cover)?
   - Note: on the cover screen the Android status bar/cutout may shrink the window. The trace showed 904×2231 (configure 344×850).
2. **Speed up the build (the user asked for this, after testing).** Of the ~20 min, ~18 is Docker exporting and unpacking a 10+ GB image; compiling takes ~2 min. Plan:
   - Change the Makefile `build-docker` to `DOCKER_BUILDKIT=1 docker build --output type=local,dest=./target-out --target <stage>` with a small `FROM scratch AS artifact` stage that does `COPY --from=build /app/target/x/release/android /`. Or use `docker build -o`.
   - That avoids `docker create/cp/rmi` and the image export.
   - The Rust/Gradle caches could also become `RUN --mount=type=cache` so rebuilds skip dependency compiles.
   - Keep the output path `target/x/release/android/...` for compatibility.
3. **Afterwards:**
   - The 16 KB alignment linker flag (`-Wl,-z,max-page-size=16384`), so the Android warning dialog goes away.
   - GPU: install Turnip/KGSL (`ninja -C /root/src/mesa-26.2.3/build-kgsl install`) and test `vulkaninfo`.
   - Brave Origin Nightly.
   - Steam with FEX.
   - Multi-display (plan in architecture.md).
   - The notification bridge.
   - The `.proot-meta-file` visibility bug, which breaks `update-ca-trust`.

### Testing tips learned this session
- The session only starts with the phone **awake and the app in the foreground**. Check `adb shell dumpsys power | grep mWakefulness=`; if it says Dozing, ask the user to unlock.
- When waiting for a new session, compare against the log's "starting Plasma Mobile" timestamp. The old log stays until the new session starts.
- Run `kscreen-doctor -o` with `XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-2 QT_QPA_PLATFORM=wayland` via ld-exec to see KWin's mode and scale.
- Protocol tracing: `WAYLAND_DEBUG=client` on KWin only, plus `unset WAYLAND_DEBUG` at the top of `localdesktop-plasma-mobile-shell`. Filter with grep and pipe to `/tmp/kwin-wayland-debug.log`. Edit guest scripts by pushing a script file through `/sdcard/Download` → `run-as cat >`; quoting through adb is fragile.

## Addendum: work done 2026-10-12 15:30–16:12, after the PAUSED notes (reconstructed 2026-10-13 from file mtimes and diffs)
- **Fast Docker build is DONE.** The Makefile `build-docker` runs `DOCKER_BUILDKIT=1 docker build --target artifact --output type=local,dest=./target .`. The Dockerfile has BuildKit cache mounts (cargo registry/git, gradle, target) and a `FROM scratch AS artifact` stage that exports only the APK to `target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk`. `target.prev/` is the old output, kept as a backup.
- **Touch rework, installed at 16:12:55:**
  - New config `[input] native_touch` (`config.rs` InputConfig) forwards real `wl_touch` (`CentralizedEvent::NativeTouch`) instead of mouse emulation.
  - The guest `/etc/localdesktop/localdesktop.toml` sets `native_touch = true` for Plasma Mobile.
  - Pixel-delta scroll now reports `AxisSource::Finger` (1:1 plus kinetic).
  - `TouchCancel` carries the gesture mode.
- The 16 KB alignment flag already exists in `patches/xbuild/xbuild/src/cargo/mod.rs:317`. Check whether the Android warning still shows.
- 2026-10-13 14:02: the session started fine with the home screen visible. Screenshots need `screencap -d <id>`, because there are multiple displays.

## 2026-10-13 session: invisible app grid/search FIXED (llvmpipe SIGILL root-caused)
- **Symptom:** the app drawer, search results and home-screen icons were invisible but still tappable.
- **Cause:** `QT_QUICK_BACKEND=software` can't draw ShaderEffects. Folio and mobileshell draw their grids through MultiEffect, `layer.effect` and OpacityMask (FlickableOpacityGradient).
- **Why software was forced:** llvmpipe died with SIGILL. Root cause:
  - On aarch64, Mesa's gallivm sets only `-mcpu=getHostCPUName()`, with no MAttrs.
  - LLVM reads "CPU part" from /proc/cpuinfo and gets `cortex-x3` (0xd4e), which implies SVE2.
  - The Snapdragon 8 Gen 2 kernel has SVE disabled (no `sve` in Features), so the JIT emitted SVE (`insn 04a14001` in an anon r-x map) and crashed.
- **Fix (app, `src/android/proot/process.rs`):**
  - `masked_cpuinfo()` checks the real cpuinfo once per app start. If it has no `sve` Feature, it writes `$ARCH/proc/.cpuinfo` with every core as implementer 0x41 / part 0xd41 (Cortex-A78) and binds it over /proc/cpuinfo.
  - Verified: LLVM reports cortex-a78, es2gears runs, and a MultiEffect QML test renders.
- **Fix (guest, `/usr/local/bin/startplasmamobile-localdesktop`):**
  - Dropped `QT_QUICK_BACKEND=software` and kept `KWIN_COMPOSE=Q`, so KWin still composites with QPainter.
  - Backup: scratchpad only. The old line was `export KWIN_COMPOSE=Q QT_QUICK_BACKEND=software`.
  - The Xfce `launch` line in localdesktop.toml still sets `QT_QUICK_BACKEND=software`. It can be dropped too.
- Verified by screenshot: the app drawer grid with icons and labels, the blurred background, and search ("fire" shows Firefox).
- **Build:** the fast Docker build took **1m50s** (was ~20 min). Installed at 14:19 with `adb install -r`.
- **Tools:**
  - `/tmp/sigill.so` in the guest is an LD_PRELOAD SIGILL handler that prints the pc, the instruction word and the map. Use it where gdb can't attach.
  - `ld-exec` doesn't add the cpuinfo bind. To test GL from ld-exec, add `--bind=$R/proc/.cpuinfo:/proc/cpuinfo`.
- **Drag-as-scroll: RESOLVED.** The user confirmed dragging works. It was fixed by the 2026-10-12 native_touch work (`[input] native_touch = true` → wl_touch), which was not documented at the time.
- **Next:**
  - User tests GL performance, the keyboard, rotation, fold, and whether the 16 KB warning dialog still appears.
  - Turnip GPU is still the real performance fix.
  - Commit after asking.

## 2026-10-13 (afternoon): GPU OpenGL working (Zink on Turnip/KGSL), shell drawn by the Adreno 740
- **Installed** `ninja -C /root/src/mesa-26.2.3/build-kgsl install` → `/opt/turnip-kgsl`.
  - `vulkaninfo` fails ("I can't KHR_display"): it enables VK_KHR_display, which the KGSL backend rejects. This is harmless.
  - `vkcube` reports "Turnip Adreno (TM) 740, Vulkan 1.4".
- **Presenting:**
  - KWin (QPainter) and the app compositor offer only wl_shm, with no linux-dmabuf, so Vulkan swapchains hang without `MESA_VK_WSI_DEBUG=sw`. That option renders on the GPU and copies each frame into wl_shm.
- **GL via Zink. Two routes were tried:**
  - `GALLIUM_DRIVER=zink`: EGL skips its Zink retry and takes the swrast path with Zink as the driver, presenting through EGL's wl_shm readback. **The frames come out BLACK.** That is what blacked out the whole session at 14:28 (since reverted).
  - **Kopper (works):** EGL uses kopper (Vulkan WSI) only when `Options.Zink && ForceSoftware`, i.e. `MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_ALWAYS_SOFTWARE=1`. Zink's `choose_pdev` also reads LIBGL_ALWAYS_SOFTWARE, then demands a CPU device ("CPU device requested but none found").
  - **Shim:** `/usr/local/lib/zink-kopper-shim.so` (source `/usr/local/src/zink-kopper-shim.c`) is an LD_PRELOAD `getenv` wrapper. It hides LIBGL_ALWAYS_SOFTWARE when the caller is in libgallium.
  - Verified: es2gears and a Qt MultiEffect test render on the GPU.
  - Cleaner long-term options: a Mesa patch, either "zink: fall back to a GPU when no CPU device exists" or "EGL wayland: use kopper when Zink is requested and there is no DRM fd". Upstream-able.
- **Session:** the env block is in `/usr/local/bin/localdesktop-plasma-mobile-shell` before `dbus-update-activation-environment`, so plasmashell, launched apps and D-Bus-activated apps get it.
  - It is guarded on the ICD and the shim existing.
  - KWin itself stays `KWIN_COMPOSE=Q` and doesn't get the env.
  - Backups: scratchpad only. To revert, delete the `if [ -r "$TURNIP_ICD" ]` block.
  - `startplasmamobile-localdesktop` is back to its pre-zink state (no GPU env for KWin).
- Verified by screenshot (14:51): the app drawer renders correctly; plasmashell maps libvulkan_freedreno and the shim.
- **Unexplained:** under the `ld-exec` + fake-cpuinfo wrapper, llvmpipe es2gears still SIGILLs, and the LD_PRELOAD handler doesn't fire (LLVM probably installs its own handler). Inside the app session llvmpipe was stable (plasmashell ran 7+ min). Matters only for the non-Turnip fallback.
- **Next:**
  - The user compares GPU vs CPU speed.
  - Possible further gains:
    - KWin GL compositing via Zink (would need the same env for KWin; currently QPainter).
    - dmabuf/AHardwareBuffer import in the app compositor, to avoid the wl_shm copies.
  - Then Brave and Steam/FEX (Vulkan now available via Turnip).

## 2026-10-13 (late afternoon): KWin GL not possible; zero-copy deferred; Tux icon
- **Change #1, KWin GL compositing via Zink: NOT POSSIBLE without KWin patches.**
  - With the GPU env and no `KWIN_COMPOSE`, KWin silently stays QPainter and maps no gallium/freedreno.
  - KWin 6.7's nested Wayland backend only does GL with a GBM allocator (`DrmDevice`, `GbmGraphicsBufferAllocator`) on a DRM device discovered through the host's linux-dmabuf. Android exposes no DRM device.
- **Launcher re-layout (tested working 15:14):**
  - The GPU env block (Turnip ICD, `MESA_VK_WSI_DEBUG=sw`, `MESA_LOADER_DRIVER_OVERRIDE=zink`, `LIBGL_ALWAYS_SOFTWARE=1`, shim appended to LD_PRELOAD via `$GPU_PRELOAD`) moved from `localdesktop-plasma-mobile-shell` into `startplasmamobile-localdesktop`, so everything inherits it.
  - `KWIN_COMPOSE=Q` is explicit, with a comment explaining why.
  - The Xfce fallback also unsets `MESA_LOADER_DRIVER_OVERRIDE` and `LIBGL_ALWAYS_SOFTWARE`.
  - Backups: scratchpad only (`*.pre-kwingl`).
- **Change #2, zero-copy (decided 10-13): deferred as a separate side project.** The user agreed it goes well beyond Local Desktop. It needs:
  - App work: AHardwareBuffer allocation and import. The Adreno EGL has `EGL_ANDROID_get_native_client_buffer` but **no** `EGL_EXT_image_dma_buf_import`, so the app must allocate and lend dma-bufs.
  - A Mesa WSI fork or a custom protocol.
  - A KWin fork (linux-dmabuf without DRM).
  - First step, if pursued: a proof of concept with one AHardwareBuffer that Turnip renders into, shown by the app.
- **Android launcher icon is now Tux:**
  - `assets/icon.png` is 512×512, transparent, with Tux ~300 px tall and centred (adaptive-icon safe zone over the white background from `patches/xbuild/.../ic_launcher.xml`).
  - Rendered from `/usr/share/inkscape/branding/tux.svg` (PC) with inkscape + magick.
  - Tux © Larry Ewing (lewing@isc.tamu.edu) made with The GIMP. His terms ask for acknowledgement if someone asks; mention it in README/credits when committing.
  - Built (1m47s) and installed 15:18; verified Tux in the APK's mipmap.
  - 15:23: session verified after the install (direct KWin, `KWIN_COMPOSE=Q`, plasmashell on Turnip via the shim, home screen renders). The launcher icon still needs checking by eye; the phone launcher may cache the old one until reboot.

## 2026-10-13 (evening): Brave Origin Nightly 1.99.25 installed and running
- **AUR `brave-origin-nightly-bin`** (maintained by Brave) supports aarch64. The arm64 `.deb` was downloaded on the PC (sha512 matches the PKGBUILD).
- **Building in the guest failed twice:**
  1. `DownloadUser alpm` user missing → created it with `systemd-sysusers /usr/lib/sysusers.d/alpm.conf`.
  2. `/etc/passwd` and `/etc/group` were 0700, so non-root users couldn't resolve any name → `chmod 644` fixed resolution (proot's `ls` still shows 0700).
  3. Then the **`.proot-meta-file` visibility bug** broke makepkg's `cp -a` ("File exists" on `.proot-meta-file.*.meta`).
- **Workaround: build on the PC** in `docker run archlinux` with base-devel, CARCH=aarch64, `!debug` and `--nodeps`. makepkg verified the sums.
  - Pushed the `.pkg.tar.zst` to the guest pacman cache, then `pacman -U` (the guest checks deps; gtk3, nss, alsa-lib, libxss and fonts were already installed).
  - **Use this route for any AUR -bin package until the proot meta bug is fixed.**
- **Flags:** `/root/.config/brave-origin-nightly-flags.conf` contains `--no-sandbox`, because proot has no userns or setuid helper.
  - The AUR wrapper passes the flags file as ONE quoted argument, so keep a single flag there or add a wrapper.
  - Chromium picks Wayland itself (ozone wayland; `drm_render_node` Permission denied is harmless).
- **Verified by screenshot:** a native Wayland window under KWin, the toolbar, and plasma-keyboard popping up for the URL bar.
  - The "Brave quit unexpectedly" prompt came from ld-exec's `--kill-on-exit` killing an earlier test run.
- **Known:** Brave's GPU process exits 3× at start (exit_code=256), then Brave carries on with software compositing. Investigate later (brave://gpu; maybe `--ozone-platform=wayland --use-gl=angle --use-angle=vulkan` on Turnip, or `--disable-gpu`).
- Network inside the session is untested; ld-exec (run-as) has none, so the user should check browsing from the app.
- **Brave GPU process (16:30):** the separate GPU process always exits with status 1 and no log, under every config tried:
  - default
  - ANGLE Vulkan
  - ANGLE GLES
  - without the GPU env
  - With `--in-process-gpu` there are 0 crashes in all three configs, so it's a proot child-process problem, not a graphics one.
  - Applied via per-user desktop overrides `/root/.local/share/applications/{brave-origin-nightly,com.brave.Origin.nightly}.desktop` (Exec + `--in-process-gpu`). These survive package upgrades but won't pick up new desktop-file content.
  - Which renderer it actually uses (Turnip via Zink, or SwiftShader) is still to be read from brave://gpu.
  - Chromium trims whitespace per argv, so the wrapper's single-argument flags file works for exactly ONE flag.
- Also observed 16:33: network works inside the session (the user's Brave loaded google.com); landscape rotation works.
- Space 16:28: /data 781 GB free; guest 7.7 GB; old backup tar 6.6 GB still on /sdcard. Steam+FEX estimate ≈ 5–9 GB before games.
- **Video test URL (the user's YouTube test, 10-13):** https://www.youtube.com/watch?v=KUUuxkICOLw (Radxa Dragon Q6A vs Raspberry Pi 5 video).
  - Baseline, fullscreen in landscape, software mode: Brave gpu-process ~94% CPU (`--use-gl=disabled` after 3 crashes), renderer ~37%, surfaceflinger ~33%, app ~18%, KWin ~9%. Frames skip.
  - Memory: 462 MB free, 5.1 GB swap used.
- **Brave GPU ROOT CAUSE FOUND AND FIXED (16:45–16:55): libpci calls `exit(1)`.**
  - Chromium probes the GPU through libpci. Android SELinux makes `/sys/bus/pci/devices` unreadable and `/proc/bus/pci/devices` doesn't open, so libpci's default error handler exits.
  - Found with `/tmp/exitbt.so` (an LD_PRELOAD exit/_exit backtrace shim; source in the scratchpad and the guest `/tmp/exitbt.c`). Backtrace: libpci.so.3 → exit(1).
  - **The `--in-process-gpu` idea was WRONG:** it just made the whole browser die. Its "0 crashes" meant no separate process existed to crash. My first "dies in 2 s" test also lacked `XDG_SESSION_TYPE=wayland`, which made Chromium try X11.
  - The desktop overrides were removed; Brave launches with package defaults again.
  - **Fix (app, `process.rs`):** `empty_pci_bus()` creates `$ARCH/sys/.pci/devices` (empty) and binds `$ARCH/sys/.pci` over `/sys/bus/pci`, so libpci sees zero devices.
  - Verified before the APK change with a hand-built proot: GPU process crash count 0; it maps Mesa libEGL/libEGL_mesa, libgallium (Zink) and Turnip's libvulkan_freedreno; renderers no longer get `--disable-gpu-compositing`.
  - APK built (1m42s) and installed 16:55; the session's proot has the bind, and `lspci` in the guest returns rc 0 with no devices.
  - `~/bin/ld-exec` now also binds `.cpuinfo` and `sys/.pci`, matching the app.
  - Next: the user plays the test video (URL above); compare CPU with the 94% gpu-process baseline.
- **Brave video after the PCI fix (17:00, test URL fullscreen in landscape):**

  | | Software (before) | GPU (after) |
  |---|---|---|
  | gpu-process CPU | 94% | 92% |
  | renderer CPU | 37% | 12% |
  | session proot (ptrace syscall translation) | — | ~52% |
  | MemAvailable | 462 MB | 2.6 GB (after the restart) |

  - The hot thread is `VizCompositorThread` (~87% user, ~8% sys).
  - **Hypothesis (unconfirmed):** Ozone/Wayland has no DRM render node or dmabuf, so viz presents by reading frames back into wl_shm on the CPU. That's the same copy cost as zero-copy project #2.
  - To confirm: brave://gpu "Compositing" line (needs the user's screen). Not done yet.
  - simpleperf can't attach (even with `security.perf_harden 0`, restored to 1 afterwards; most likely because proot already ptraces the process).
  - Options:
    - 720p.
    - Try `--use-gl=angle --use-angle=vulkan` and compare viz CPU.
    - Zero-copy project.
    - Reduce proot ptrace cost (e.g. proot's seccomp acceleration — check whether the app's proot uses `PROOT_NO_SECCOMP`).

## PAUSED 2026-10-13 ~17:05. Resume here.
### State (all installed and working)
- **APK** (installed 16:55, persistent key) adds:
  - the masked `/proc/cpuinfo` (llvmpipe SVE fix)
  - the empty `/sys/bus/pci` bind (Chromium libpci exit fix)
  - the Tux launcher icon
- **Guest session:** Plasma Mobile direct on KWin (`KWIN_COMPOSE=Q`, CPU compositing; KWin GL is impossible without a DRM device).
  - Shell and apps use GPU GL: Zink on Turnip/KGSL (`/opt/turnip-kgsl`) via kopper, with `MESA_VK_WSI_DEBUG=sw` and `/usr/local/lib/zink-kopper-shim.so`. The env lives in `/usr/local/bin/startplasmamobile-localdesktop`.
- **Brave Origin Nightly 1.99.25:** GPU process working, `--no-sandbox` in `/root/.config/brave-origin-nightly-flags.conf`. Video works but is CPU-heavy (see the table above).
- **Guest fixes this session:**
  - `alpm` user created.
  - `/etc/passwd` and `/etc/group` set to 644.
  - FlickableOpacityGradient.qml restored to stock.
- **Debug tools in the guest `/tmp`:** `sigill.so`, `exitbt.so` (+ `.c`), plus test scripts (`gltest.sh`, `bgl.sh`, `bshow.sh`, ...). All safe to delete.
- **Uncommitted in `~/localdesktop`:** everything from 10-12, plus:
  - `process.rs` (cpuinfo mask, empty PCI bus)
  - `assets/icon.png` (Tux; credit Larry Ewing / The GIMP in the README when committing)
  - Makefile and Dockerfile (fast build, ~1m45s)
  - **Ask before committing.**
- **On the phone:** `/sdcard/Download/localdesktop-files-backup.tar` (6.6 GB) can be deleted once the user is happy.

### NEXT SESSION
1. **Steam + FEX** (the user deferred it to its own session).
   - Space is fine: 781 GB free, guest 7.7 GB, estimate ≈5–9 GB before games.
   - Plan: FEX-Emu + an x86-64 RootFS; Valve's ARM64 Steam client (or x86 Steam under FEX); Proton 11.
   - Vulkan is available via Turnip (needs `MESA_VK_WSI_DEBUG=sw` and the ICD env, which the session already exports).
   - For prebuilt AUR packages, build on the PC in `docker run archlinux` (CARCH=aarch64) because of the proot meta bug.
2. Optional:
   - Confirm the Brave compositing hypothesis via brave://gpu.
   - Try ANGLE-Vulkan.
   - Check proot seccomp acceleration to cut the ~52% ptrace overhead.
3. Backlog:
   - The `.proot-meta-file` visibility bug (breaks makepkg `cp -a` and update-ca-trust).
   - Zero-copy side project (AHardwareBuffer + Mesa WSI + KWin).
   - Notification bridge.
   - Multi-display.
   - Remaining user checks: keyboard in apps, fold to the cover screen, 16 KB dialog.
   - Review Droid Deck (https://github.com/Droid-Deck/DroidDeck) and reuse parts of its tech stack for graphics, audio, or anything else Local Desktop lacks (added 2026-10-23).

## Droid Deck review (2026-10-23)
Cloned at `~/src/DroidDeck` (commit 8ebba02). It is GPL-3.0, the same as Local Desktop, so code can be reused with credit. It runs Valve's ARM64 Steam client + ARM64 Proton (FEX loaded in-process via arm64ec) under proot on Adreno: this is our Steam+FEX plan, already solved.

**Worth taking (ranked):**
1. **GPU KWin compositing.** We assumed it was impossible without DRM. DroidDeck creates a fake DRM render node:
   - fake sysfs tree (`/sys/dev/char/<maj:min>`, `/sys/class/drm/renderDN`, uevent `DRIVER=kgsl-3d0`)
   - binds `/dev/kgsl-3d0` to `/dev/dri/renderD<minor>`
   - preload `drm.c` emulates GEM handles over KGSL dmabufs
   - a KWin patch selects `KWIN_RENDER_NODES` when nested dmabuf feedback names no device
   - session env: `KWIN_COMPOSE=O2 GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=true`
   - files: `app/src/main/java/.../runtime/LinuxRuntime.java:~250-300`, `tools/linuxfs/preload/drm.c`, `tools/desktop-kde/patches/0002-*.patch`, `tools/linuxfs/desktop/droiddeck-desktop:77`
   - they pin **Mesa 26.2.4**: 26.2.5 crashes in Zink shader compilation with Turnip. Check our guest's Mesa.
2. **Patched proot** (`tools/proot/patches` 0001–0017, on termux/proot 4dba3afb):
   - seccomp-by-argument and ioctl/futex off the trap list
   - Android hardlink/execstack/openat2/clone3 fixes
   - **fast-path preload** (patch 0014 + `tools/proot/fastpath`): stat 25 µs→0.6 µs
   - directly addresses our ~52% ptrace overhead. Our `assets/libs/arm64-v8a/libproot.so` is presumably stock Termux.
3. **Steam/FEX recipes** (`docs/development/x86-64-steam-client.md`, `game-environment.md`, `tools/fex`):
   - FEX patches
   - SysV IPC shim (`preload/sysv.c`; Android has no semget)
   - tracer-hiding for tier0 (`preload/tracer.c`)
   - user-namespace probe shim for pressure-vessel
   - VKD3D/DXVK env defaults
   - gamescope ARM64 patches
4. **Preload shims** (`tools/linuxfs/preload/*.c`) for things we hit or will hit: `robust.c`, `usr2.c`, `writewatch.c` (Wine), `kgslid.c` (GPU id spoof), `net.c`/`netif.c`, `pulseguard.c`.
5. **Zero-copy:** `app/src/main/cpp/waylandcomp/src/ahb_swapchain.c` and `wl_dmabuf.c` is an AHardwareBuffer present path in their compositor (C, not Rust). It's a reference for our zero-copy backlog item.
6. **Audio:** we already have PipeWire→AAudio. They also have a PulseAudio AAudio sink (`tools/aaudio-sink`) and DirectAudio sink. Only worth a look if latency or bugs show up.

**Phone prerequisite for Steam:** Android 16 phantom-process killer. `settings_enable_monitor_phantom_procs` is currently `null` (default = on). Turn on Developer options → "Disable child process restrictions" before Steam.

## Stack order agreed 2026-10-23: proot → KWin GPU compositing → Steam/FEX

### Step 1 DONE (2026-10-23 ~09:10): patched proot
- DroidDeck's 19 proot patches applied to our vendored termux/proot (ab2e3464). All applied cleanly in order. Plus our own `0018-localdesktop-keep-statx-exit-for-stat-fixups.patch`:
  - Their 0013 drops the statx exit stop on kernels 5.8 and newer.
  - That broke fake_id0 (`--root-id`) and link2symlink: files showed as owned by uid 10084 and hard links had nlink 1. Found by bisecting.
- Repo changes (uncommitted):
  - `patches/build-proot-android/{patches/, build-patched.sh, README.md}`
  - patched `build/proot/src`
  - new `assets/libs/arm64-v8a/libproot{,_loader}.so`, now 16 KB aligned
  - Build: `patches/build-proot-android/build-patched.sh` (NDK r27c in `~/Android/android-ndk-r27c`). Reproducible.
- **Measured** (ld-exec flags, screen awake), old → new:
  - ioctl 51 µs → 0.4 µs
  - fstat 587 → 84 µs
  - stat 305 → 126 µs
  - access 708 → 105 µs
  - uname 414 µs → 0.3 µs
  - pthread create 655 → 231 µs
  - fork+exec+wait 2.8 → 1.1 ms
- **fake_id0 (`--root-id`) is still the biggest per-call cost:** about 85 µs for every fstat, versus 1.2 µs without it.
  - Future idea: run the session as a non-root user with `-i` at the app uid, so DroidDeck's identity mode applies.
- APK installed. Plasma Mobile and the app drawer verified by screenshot. Correctness checks: chown/chmod/ln/pacman -Qk.
  - The old proot silently lost chmod; the new one keeps it.
- **Fast path (patch 0014) is NOT enabled yet.** It needs `PROOT_FASTPATH` plus DroidDeck's `tools/proot/fastpath/fastpath.c` preload. Optional follow-up.
- **Gotcha:** when the phone is folded, screenshots must use the cover display: `screencap -d 4630946481096930692`. The inner display is `4630947175568309891` and captures black when it's off. The cover screen works fine (a remaining user check is now done).
- Benchmark binary: guest `/tmp/sysbench` (from DroidDeck `tools/proot/bench/sysbench.c`).

### Step 2 NEXT: KWin GPU compositing (see "Droid Deck review" item 1)

### Step 2 status (2026-10-23 ~09:40): KWin OpenGL compositing WORKS, but opt-in and OFF
- **Guest:**
  - Fake render node: `process.rs` `kgsl_render_node()` binds `/dev/kgsl-3d0` → `/dev/dri/renderD0` plus fake `/sys/dev/char`, `/sys/class/drm`, after DroidDeck.
  - `/usr/local/lib/kgsl-drm-shim.so` (source `/usr/local/src/kgsl-drm-shim.c`: DroidDeck drm.c + an ioctl wrapper).
  - Wrapper `/usr/local/bin/kwin-localdesktop-gl` (KWIN_COMPOSE=O2, KWIN_RENDER_NODES).
  - **NO KWin rebuild needed for 6.7.5:** our host names the device in the dmabuf feedback.
  - Switch: `touch /etc/localdesktop/kwin-opengl` (currently REMOVED, so the session uses QPainter).
  - Shell launcher restores client env (`LIBGL_ALWAYS_SOFTWARE=1 MESA_VK_WSI_DEBUG=sw`).
  - Backups: `startplasmamobile-localdesktop.bak-1023`.
- **Host:**
  - `compositor.rs`: linux-dmabuf global (linear ARGB/XRGB/ABGR/XBGR, main device = kgsl 479:0).
  - smithay patch `GlesRenderer::import_dmabuf_by_copy`: mmap + DMA_BUF_IOCTL_SYNC + TexImage2D, used because Android EGL has no dmabuf import.
  - Verified: `Compositing Type: OpenGL`, 15 dmabuf fds in the app, 0 shm. Home and drawer render correctly.
- **Also verified:** Mesa GBM + EGL (GLES on "zink Vulkan 1.4 (Turnip Adreno 740)") on the fake node. GPU-rendered linear dmabufs are mmap-readable (correct pixels).
- **vulkaninfo is NOT a valid probe:** it enables VK_KHR_display, which Turnip/KGSL fails, so the whole init fails.
- **GPU tests must run in-session:** run-as (`runas_app` domain) can open kgsl but its GPU init fails.
  - New `~/bin/ld-run 'cmd' [timeout]` runs in-session via the `/tmp/ld-run` runner loop in the session script.
  - For KWin D-Bus: `export $(tr "\0" "\n" < /proc/$(pidof kwin_wayland)/environ | grep ^DBUS_SESSION_BUS_ADDRESS)`.
- **CPU over 16 drawer open/close** (scratch `cpubench.sh`):

  | | kwin | plasmashell | app | total |
  |---|---|---|---|---|
  | QPainter | 2.1 s | 1.8 s | 4.8 s | ≈9.0 s |
  | GL | 1.35 s | 1.7 s | 6.3 s | ≈9.7 s |

  - **Net loss:** the host's full-frame copy from KGSL memory (likely uncached) costs more than KWin saves.
- **To make it pay off (options):**
  - (a) Damage-only upload into one texture per surface.
  - (b) Clients → KWin zero-copy: drop `MESA_VK_WSI_DEBUG=sw` when KWin offers dmabuf. Saves the plasmashell/Brave readbacks.
  - (c) Host zero-copy: Android Turnip in the app via adrenotools (DroidDeck `vk_present.c`/`ahb_swapchain.c`); the system Adreno Vulkan lacks `VK_EXT_external_memory_dma_buf`.
- **Icons:** they rendered fine after a fresh start unfolded. The tiled icons seen after unfold may be a rescale-time bug; recheck later.

## PAUSED 2026-10-23 ~09:45. Resume here (the user is back in about an hour).
- **State:**
  - Phone running the latest APK: patched proot + render node + linux-dmabuf.
  - Session on QPainter, since `/etc/localdesktop/kwin-opengl` is absent.
  - The phone was open (unfolded) during testing.
- **Next:** option (b) from Step 2: clients hand dmabufs to GL KWin. Then rerun `cpubench.sh` (in the session scratchpad, so it may be gone; it's trivial to recreate: 8× swipe up/down while sampling `/proc/<pid>/stat` utime+stime of kwin, plasmashell, app, proot).
  - To try it: `touch /etc/localdesktop/kwin-opengl`, then drop `MESA_VK_WSI_DEBUG=sw` (and possibly `LIBGL_ALWAYS_SOFTWARE` + the kopper shim) for clients in `localdesktop-plasma-mobile-shell`, so Zink presents via KWin's linux-dmabuf.
  - Watch for black frames: the old note at 14:28 said `GALLIUM_DRIVER=zink` without kopper went black, but that was with no dmabuf anywhere.
- **Then:** (a) damage-only upload, or (c) Android Turnip in the host (zero-copy). Then Step 3, Steam/FEX.
- **When resuming:** check `adb devices` (re-authorize if needed), wake the phone, open the app, then use `ld-run` for in-session/GPU tests.
- **Still uncommitted; ask before committing:** everything in `~/localdesktop`, including:
  - `patches/build-proot-android/*`
  - `assets/libs/arm64-v8a/libproot*.so`
  - `src/android/proot/process.rs`
  - `src/android/backend/wayland/compositor.rs`
  - `patches/smithay/src/backend/renderer/gles/mod.rs`

### Step 2 continued (2026-10-23 ~11:00): GL KWin is now a small net win, and ON
- **(b) done:** clients present dmabufs to KWin.
  - `localdesktop-plasma-mobile-shell` keeps `LIBGL_ALWAYS_SOFTWARE=1` but no longer sets `MESA_VK_WSI_DEBUG=sw` when KWin is GL.
  - es2gears test, ~57 fps in every mode, CPU per 8 s: A (sw copy) 0.54 s; B (Zink kopper dmabuf) 0.37 s; C (EGL dri2 dmabuf + kgsl shim) 0.39 s. B was chosen.
  - plasmashell holds ~40 dmabufs.
- **(a) done:** smithay copy-import keeps one texture per surface and uploads only the damaged rects. `DMA_BUF_IOCTL_SYNC` removed: KGSL maps uncached, no stale data seen, and it saved 10–17%.
- **Totals now:**

  | Benchmark | QPainter | GL |
  |---|---|---|
  | drawer, 16 cycles | ≈9.0 s | ≈8.4 s |
  | 10 Hz terminal clock, 15 s | ≈7.4 s | ≈7.3 s |

  - Guest side (kwin + clients) is down ~45%. The app (host) is up.
- **simpleperf (app is debuggable):** `adb shell simpleperf record --app app.polarbear --duration 10 -f 2000 -o /data/local/tmp/perf.data`.
  - Adreno GLES driver 44% (its upload copy from the mmap), libc 11% (malloc/free and locks, memmove <1%), libgui 9%, our code 15%.
  - The remaining host cost needs **(c) zero-copy import** (Android Turnip in the app via adrenotools, like DroidDeck `vk_present.c`).
- **`/etc/localdesktop/kwin-opengl` is ON.** Remove it and restart the app to return to QPainter.
- Benchmarks in the session scratchpad: `cpubench.sh` (drawer swipes) and `smallbench.sh` (xfce4-terminal clock; uses the ld-run folder directly).

### Step 2: frame pacing (#4) DONE; control build archived (2026-10-23 ~11:40)
- **Measurement tools:**
  - `~/bin/ld-framebench <label>`: idle / drawer / es2gears phases. Combines SF timestats, the app's frame-stats log and CPU. Output goes to `~/localdesktop-bench/`.
  - App frame-stats instrumentation: enabled by `run-as app.polarbear touch files/frame-stats`.
- **Baseline findings:**
  - The app redrew 120×/s forever: ~20% of a core at idle.
  - The output advertised a hard-coded 60 Hz (`run.rs`).
  - Frame callbacks were sent before dispatch (+1 frame latency).
  - KWin content was 49–59 fps in the drawer and 48–59 in gears.
- **Fixes:**
  - `ndk::refresh_rate_mhz` (`Display.getRefreshRate` → 120000).
  - `wake.rs` thread (polls listener + display fds → `AppUserEvent::WaylandReady`).
  - `State.needs_redraw` set on commit; the redraw loop continues only while there are commits or touches.
  - `ControlFlow::Poll` → `Wait` in `winit_backend.rs` (Poll made the event loop spin at 100% once redraws stopped).
  - Dispatch before draw; frame callbacks sent right after dispatch (v3). This beat sending them after the swap (v2): gears 68–90 vs 49–68.
- **Results (v3 vs baseline):**
  - Idle app CPU 1.96 s → 0 per 10 s.
  - Drawer KWin fps 49–59 → 45–77; app CPU 3.55 → ~1.9 s.
  - Gears 48–59 → 68–90 fps; app CPU 8.2 → ~5.7 s.
- **Remaining limits:**
  - Host upload ~4.2 ms/frame.
  - KWin CPU (~30% of a core at 80 fps).
  - The drawer is likely bound by plasmashell (Qt Quick on Zink) rendering.
- **Control build archived:** `~/localdesktop-bench/control-v3/` (APK + source diff + untracked tarball + guest scripts + benchmarks + README). The installed APK was verified identical.
- **Next:** host zero-copy options. (1) Probe the stock Adreno `GL_EXT_memory_object_fd` / `VK_KHR_external_memory_fd` opaque import of KWin's dmabufs. (2) Android Turnip in-app (2a: GPU copy into AHB; 2b: Vulkan presenter). (3) Host-allocated AHB buffers. Compare each against control-v3 with `ld-framebench`.
- **Committed (2026-10-23 ~11:45):** `6038c68` on new branch `perf/gpu-compositing-pacing` (not pushed). README Credits added (Tux, DroidDeck).

## 2026-10-23 afternoon: Brave crash + DNS fixed; sync research; APK now carries the guest (IN PROGRESS)
- **Brave "crash on open" (FIXED):** since the fake `/dev/dri/renderD0`, Chromium's GPU process opened it and `drmGetVersion` failed (KGSL has no DRM_IOCTL_VERSION) → `LOG(FATAL)` in `drm_render_node_handle.cc` ×6 → "GPU process isn't usable. Goodbye." `kgsl-drm-shim.c` now answers DRM_IOCTL_VERSION (name "kgsl"); plasmashell children already preload it via KWin's env.
- **"No internet" in Brave (FIXED):** the guest's resolv.conf had only 1.1.1.1, and on this LTE/Tailscale setup TCP to 1.1.1.1 / 8.8.8.8 over IPv4 is refused even from adb shell (other IPv4 hosts and IPv6 fine). The app now writes `/etc/resolv.conf` at every start from Android's active network (`ConnectivityManager.getLinkProperties().getDnsServers()`, follows the VPN → 100.100.100.100), fallback IPv6-first list. Opt out: `/etc/localdesktop/keep-resolv-conf`.
- **Green flashes in fullscreen YouTube:** intermittent; Brave hands KWin dmabufs (24 dmabufs + sync_files in its GPU process). Kernel 5.15 has **no** `DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE` (probed: ENOTTY), KGSL has no DRM syncobj → no implicit or explicit cross-process sync. `MESA_VK_WSI_DEBUG=sw` restored for clients (it doubles as a sync: the copy happens after the GPU finishes). Brave flag override tried and removed (didn't decide it; files in `~/.local/share/applications/disabled/`).
- **Green detector:** `frame_stats.rs` samples every copied KWin dmabuf (32×18 grid; R,B<16 & G≥48 on ≥25% → GREEN frame, logged). Hook: `DMABUF_COPY_PROBE` in patched smithay gles. Harness: `~/bin/ld-greentest <label> [secs]` plays `/root/greentest/gray1080.webm` fullscreen in Brave (grayscale VP9, so any green = unfinished frame).
- **Next for sync:** present-wait Vulkan layer (wait the present's semaphores → fence on CPU before handing the frame over), measured with ld-greentest; same idea for KWin → app compositor.
- **APK carries the guest now:** `guest/managed` (rewritten each start), `guest/seed` (only if missing), `guest/blobs/turnip-kgsl.tar.xz` (Mesa 26.2.3 KGSL Turnip, 2.4 MB). `guest/build-overlay.sh` (run by `make build`/`build-docker`) → `assets/guest-overlay.tar` + `assets/turnip-kgsl.tar.xz` (gitignored). `guest/build-shims.sh` rebuilds the 4 shims in the guest via adb. Stage `setup_guest_overlay` (Step 12) in setup.rs. `localdesktop-install-plasma-mobile [brave.pkg]` installs the Plasma package set + Brave and writes the toml (fresh installs stay Xfce until run).
- **Snapshot of all phone-only state:** `~/localdesktop-bench/guest-snapshot-1023/` (guest-snap.tar, Brave pkg 183 MB, package lists).
- Session flag: in-session runner now needs `/etc/localdesktop/test-runner` (created on the phone). KWin GL is the default; `/etc/localdesktop/kwin-qpainter` switches it off (the old `kwin-opengl` file is ignored).

## PAUSED 2026-10-23 ~17:00. Resume here.
### Installed on the phone (APK `~/localdesktop-bench/localdesktop-1023-commitwait.apk`)
- Everything from the "afternoon" section above, plus:
  - **kgsl-commit-wait.so** (B), preloaded session-wide: before a `wl_surface.commit`, waits for the process's last KGSL submissions (tracks IOCTL_KGSL_GPU_COMMAND / GPU_AUX_COMMAND timestamps, waits with WAITTIMESTAMP_CTXTID, 500 ms cap). `LOCALDESKTOP_COMMIT_WAIT=0` off / `=log`. Hooks wl_proxy_marshal{,_flags,_array_flags}.
  - kgsl-drm-shim is now preloaded **session-wide** (was KWin only). Fixes Brave started outside plasmashell (the test runner) dying on "Can't get version for device".
  - Frame-stats probe also classifies synctest frames: `sync[frames mixed stale]`. Green criterion is now G 48–180.
  - `/etc/localdesktop/dev-keep-managed`: the app leaves managed guest files alone at start (for testing edits). NOT set now.
- Phone flags: `files/frame-stats` (probe on), `/etc/localdesktop/test-runner` (ld-run works). Client copy `MESA_VK_WSI_DEBUG=sw` still ON.
- Uncommitted (branch `perf/gpu-compositing-pacing`, last commit 6038c68). Backup: `~/localdesktop-bench/wip-1023-evening/` (tracked.diff, guest.tar.gz, test scripts). **Ask before committing.**

### Sync investigation: findings
- **Root cause of Brave's garbage/green flashes:** Android overlays (shade, notifications, volume panel) → Brave re-creates its buffers → KWin shows a new dmabuf before Brave's GPU process drew it. Chromium renders in the GPU process but commits from the browser process, so the commit can precede the render submission. Screenshot proof: `scratchpad/shade/c4a.png` (glyph-atlas garbage in the page area).
- **Tests (all scripts in ~/bin, outputs in ~/localdesktop-bench/):**
  - `ld-greentest` (gray video / YouTube fullscreen; env PAGE, URL, KEY, TAPS, SHELL_ENV, PROFILE_SRC): runs A–F all 0 green → playback alone doesn't trigger it.
  - `ld-shadetest <label> [cycles] [flags]`: shade open/close + screenshot. **H default 4/20 garbage; I `--disable-gpu` 0/20.** This is the pass/fail test for A.
  - `ld-synctest <label> secs iter passes [env]` (guest `/root/synctest`, src `guest/tools/synctest.c`): heavy GPU frames cycling R/G/B. **L no-wait 0 mixed / M wait 0 mixed** (stale=12 in both = noise). → Same-process clients (Mesa, Plasma, KWin) are safe without a wait: KGSL runs submissions in order and they submit before committing.
- Kernel 5.15: no DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE (ENOTTY). Brave has `wp_linux_drm_syncobj_manager_v1` (+ feature `WaylandSyncobjReleaseTimeline`), NOT zwp_linux_explicit_synchronization_v1; Brave carries its own libdrm (raw ioctls). KWin uses libdrm drmSyncobj* (→ ioctls). Mesa WSI on this kernel does no wait (dma_buf_semaphore unavailable, wsi->sw off).

### Agreed order: B → A → zero-copy options 1–3
**B remaining (small):**
1. Drop `MESA_VK_WSI_DEBUG=sw` in `guest/managed/usr/local/bin/localdesktop-plasma-mobile-shell` (+ comment), rebuild, verify (frame detectors, shade test on Plasma), measure CPU vs control-v3 (`ld-framebench`).
2. Hop-2 A/B: KWin with `LOCALDESKTOP_COMMIT_WAIT=0` (touch dev-keep-managed, edit startplasmamobile on the phone, restart) + `ld-synctest` → does KWin→app need the wait?
3. Notes/commit (ask).
**A (the Brave fix): DRM syncobj emulation in kgsl-drm-shim at the ioctl level.**
- Answer DRM_IOCTL_GET_CAP SYNCOBJ / SYNCOBJ_TIMELINE; emulate SYNCOBJ_CREATE/DESTROY/HANDLE_TO_FD/FD_TO_HANDLE (incl. IMPORT/EXPORT_SYNC_FILE flags)/WAIT/TIMELINE_WAIT/RESET/SIGNAL/TIMELINE_SIGNAL/QUERY/TRANSFER/EVENTFD.
- Design: timeline state in a shared memfd (the "syncobj fd" passed over Wayland); a point imported from a real sync_file gets a watcher thread in the importing process that marks it signalled; cross-process waiters via futex/eventfd on the shared state.
- First check prerequisites: Zink exports EGL_ANDROID_native_fence_sync / Vulkan SYNC_FD on Turnip-KGSL; KWin advertises the global once the caps are answered; Chromium binds it (maybe needs `--enable-features=WaylandSyncobjReleaseTimeline`).
- Pass = `ld-shadetest` 0/20 with GPU on (baseline H 4/20).
### Other open items
- Firefox content processes segfault ("Shared memory PlatformHandle is not safe to map") — not investigated.
- Duplicate Brave drawer entry (`com.brave.Origin.nightly.desktop` is NoDisplay; check after a sycoca rebuild).
- DNS is read only at app start (network changes need an app restart).

## 2026-10-26 08:25: black screen after the commit-wait APK — FIXED on the phone, APK not yet rebuilt
- Symptom: launching the app gave a black screen. plasmashell idled with 4 threads: "Aborting shell load: The activity manager daemon (kactivitymanagerd) is not running."
- Cause: **kgsl-commit-wait.so crashed kactivitymanagerd** (SIGSEGV at NULL). Its hooks forwarded to `dlsym(RTLD_NEXT, ...)`, which can't see a libwayland that Qt's Wayland QPA plugin dlopen()ed in a local scope → NULL call. KWin and plasmashell link libwayland directly, so Friday's tests passed. **Lesson: after every install, screenshot the Plasma shell, not just KWin/synctest.**
- Fix: `wayland_function()` falls back to `dlopen("libwayland-client.so.0", RTLD_NOLOAD)` + dlsym, and aborts with a message rather than calling NULL. Rebuilt via build-shims.sh (repo `guest/managed/usr/local/lib/kgsl-commit-wait.so` is the fixed one).
- Phone: the fixed .so is copied into /usr/local/lib and **`/etc/localdesktop/dev-keep-managed` is SET** so the app doesn't restore the APK's broken copy. To finish: `make build-docker`, install, `rm /etc/localdesktop/dev-keep-managed`, restart, screenshot the shell.

## 2026-10-26 08:31: Brave segfaulted on open → kgsl-commit-wait REMOVED from the session
- Brave's browser process segfaulted at start with kgsl-commit-wait preloaded, even with `LOCALDESKTOP_COMMIT_WAIT=0` → the libwayland hooks themselves (Brave carries a static libwayland; Mesa loads the system one into the same process). Without the shim Brave runs.
- Since synctest L/M showed same-process clients need no wait (0 mixed either way), B's commit-wait has no measured benefit and broke two programs (kactivitymanagerd, Brave) → **removed from GPU_PRELOAD** in startplasmamobile-localdesktop (repo + phone). Source and .so stay in the repo; not preloaded. If a wait is ever needed, do it without hooking libwayland (e.g. Vulkan present layer).
- B's remaining value = the finding itself: Mesa clients can drop `MESA_VK_WSI_DEBUG=sw` (still to do + verify).
- Phone: `dev-keep-managed` still SET (phone files = repo files). Verified: Plasma shell up (51 threads, screenshot), Brave test launch 15 s no crash.
- Lesson (again): after any session-wide preload change, check the shell screenshot AND launch Brave.
- APK still has the old script/shim → rebuild, install, remove dev-keep-managed, verify (shell screenshot + Brave).
- **08:39 DONE:** rebuilt APK (`~/localdesktop-bench/localdesktop-1026.apk`) installed, `dev-keep-managed` removed (overlay wrote 1 file). Verified: Plasma shell screenshot, 52 threads, preload without commit-wait, Brave test launch 0 crashes. Phone and APK now match the repo. Still uncommitted (ask first).

## 2026-10-26 ~09:00: step B DONE — client CPU copy removed (A/B)
- Build IDs: `make build-docker BUILD_ID=<id>` → `/etc/localdesktop/build-id` on the device; all harnesses record it. APKs in `~/localdesktop-bench/apks/`; `~/bin/ld-abrun <id>` installs, verifies the ID + shell, runs ld-framebench ×2 (results in `ab-<id>/`). ld-framebench now runs es2gears with plasmashell's environment (the runner's env always had MESA_VK_WSI_DEBUG=sw).
- A = `ld-20261026-a-client-copy` (MESA_VK_WSI_DEBUG=sw), B = `ld-20261026-b-client-zerocopy` (unset). CPU cs per phase, 2 runs:
  - drawer: KWin 131/132 → 88/86 (−34%), plasmashell 135/134 → 118/114 (−14%), app 174/186 → 191/193 (noise); total 440/452 → 397/393 (−11%).
  - gears: KWin 459/453 → 414/439, app ~same, fps 77/76 → 77/87. (es2gears' own CPU not measured.)
  - green=0 everywhere, shell normal, no dropped frames.
- **Installed now: B.** Remaining frame-time cost: app upload ~3 ms/frame → zero-copy options 1–3 (hop 2).
- **Next: A (DRM syncobj emulation for Brave).** Uncommitted: everything since 6038c68 (ask first).
- **Committed 2026-10-26:** `5e92ae9` on `perf/gpu-compositing-pacing` (not pushed). kgsl-commit-wait moved to `guest/tools/` (not shipped; header explains why). Installed APK = build B (it still contains the unused .so, harmless; the next build drops it). Next: A.

## 2026-10-26 ~09:20: cleanup DONE
- Commits (branch perf/gpu-compositing-pacing, not pushed): 4ba7d50 stale proot tmp cleanup at app start, 8aac894 device test tools in `scripts/device/` (~/bin/ld-* are now symlinks to them), 4c59c82 README "Guest files shipped in the APK". Working tree clean.
- Installed: `ld-20261026-c-housekeeping` (= B + startup cleanup, no commit-wait). Verified: build-id, shell, Brave launch, drawer shows ONE Brave.
- Duplicate Brave cause: `~/.local/share/applications/disabled/` (XDG scans subdirs) — deleted.
- Phone cleanup: guest /tmp 247 MB → <1 MB, 70 proot tmp dirs, .bak files, kwin-opengl flag. Kept: /root/greentest, /root/synctest, /root/src (Mesa), files/frame-stats + test-runner (until A is done).
- Note: the guest overlay never deletes files dropped from the APK (removed kgsl-commit-wait by hand).
- PC: removed target.prev (929 MB), raw shade screenshots (evidence PNGs in ~/localdesktop-bench/evidence/).
- Next: A (DRM syncobj emulation for Brave). Open: Firefox content-process segfaults; DNS only refreshed at app start.

## 2026-10-26: Phase 1:1 (A, explicit sync for Chromium) — prerequisites DONE, design ready
Sources read: KWin 6.7.5 (scratchpad src-kwin), Chromium 155.0.8059.40 = Brave's (gitiles).
### Gates
- **KWin** advertises `wp_linux_drm_syncobj_manager_v1` iff `drmGetCap(DRM_CAP_SYNCOBJ_TIMELINE)` ≠ 0 (DrmDevice ctor) AND `uname().release` ≥ 6.6.19 and not 6.7.x (WaylandServer::setRenderBackend; guards DRM_IOCTL_SYNCOBJ_EVENTFD).
- **Chromium** binds it iff `uname` ≥ **6.11** (MinSupportedKernelForLinuxDrmSyncobj; eventfd + WAIT_AVAILABLE fixes). No feature flag (`WaylandSyncobjReleaseTimeline` is a class name). Bound ⇒ `SupportsExplicitSync()` ⇒ every dmabuf uses SyncMethod::kSyncobj. Today: kImplicit = no sync on this kernel (kDMAFence interop needs DMA_BUF sync-file ioctls) → the garbage.
### Calls to emulate (both via libdrm → ioctl on the renderD0 fd = /dev/kgsl-3d0)
GET_CAP(SYNCOBJ, SYNCOBJ_TIMELINE); SYNCOBJ_CREATE/DESTROY; HANDLE_TO_FD (plain → timeline fd; flag EXPORT_SYNC_FILE → sync_file); FD_TO_HANDLE (plain; flag IMPORT_SYNC_FILE); TRANSFER (binary↔timeline point); TIMELINE_SIGNAL; TIMELINE_WAIT (KWin: WAIT_AVAILABLE, timeout 0 = "materialized?"); EVENTFD (KWin flags 0; Chromium WAIT_AVAILABLE). KWin also SYNC_IOC_MERGEs real sync_files (kernel does that).
### Flow
- Acquire: Chromium GPU process makes an EGL native fence (Zink ✔) → browser process `ImportSyncFile`+`Transfer` to point N → KWin `EVENTFD(N)` → waits on CPU, then may `exportSyncFile(N)`.
- Release: KWin `moveInto(N, its release fence)` or `TIMELINE_SIGNAL(N)` → Chromium `EVENTFD(N, WAIT_AVAILABLE)` → `ExportCurrentSyncPointToSyncFd` → GPU process waits.
### Device facts (guest/tools/fenceprobe.c, 10-26)
- KGSL `IOCTL_KGSL_TIMESTAMP_EVENT` FENCE on retired ts 0 of an own draw context ⇒ real **signalled sync_file** ✔ (future ts → EINVAL). SYNC_IOC_MERGE / FILE_INFO / poll work on 5.15 ✔.
- Zink: EGL_ANDROID_native_fence_sync ✔; eglDupNativeFenceFDANDROID → real sync_file, signalled 5.6 ms after flush ✔.
### Design (kgsl-drm-shim, ioctl level)
- Timeline = shared memfd page: {magic, signalled_point (u64, futex word pair), waiter wake counter}. Timeline fd passed over Wayland = that memfd (HANDLE_TO_FD dup; FD_TO_HANDLE recognises it by fstat+magic). Per-process handle table → mapping.
- Binary syncobj (temp handles) = process-local slot holding a sync_file fd (or "signalled").
- TRANSFER binary→point N: if the fence is signalled, raise signalled_point to N; else a watcher thread poll()s the sync_file and raises it when signalled (points are monotonic; raise = max).
- TIMELINE_SIGNAL: raise directly. TIMELINE_WAIT/EVENTFD: compare signalled_point ≥ N; else futex-wait / watcher thread that writes the eventfd. WAIT_AVAILABLE treated as WAIT signalled (conservative; correct, at most a bit later).
- EXPORT_SYNC_FILE of point N: wait until signalled (CPU), then return a dup of a cached signalled KGSL fence.
- Open: **uname scope** (KWin ≥6.6.19, Chromium ≥6.11). Proposal: the shim reports e.g. 6.12.0 only to processes whose exe is kwin_wayland or Chromium-family (brave/chrome/chromium/electron), not system-wide.
- Pass test: `ld-shadetest` 0/20 (baseline 4/20), plus ld-greentest, Brave/KWin stability, CPU A/B vs ld-20261026-c-housekeeping.

## 2026-10-26 ~10:15: Phase 1:1 A (explicit sync for Chromium) — WORKS, installed
- `kgsl-syncobj.so` (guest/managed/usr/local/src/kgsl-syncobj.c), preloaded session-wide after kgsl-drm-shim. Config seed `/etc/localdesktop/explicit-sync` (enabled, min_kernel 6.12, programs kwin_wayland brave chrome chromium electron steamwebhelper). uname override only for listed programs while the real kernel < floor; logged once per process.
- Unit test `guest/tools/syncobjtest.c` (two processes, real Zink GPU fence): 18/18 PASS.
- Live: KWin + Brave each hold 4 `memfd:ld-syncobj` timelines while playing video; no crashes.
- **Pass test: ld-shadetest 0/40 garbage** (baseline H 4/20). Evidence: shade-J-explicit-sync/.
- CPU: control C re-run today = same as D within noise (drawer identical; gears app 654/671 vs 653/771/752 — es2gears doesn't use the emulation). Sat's B numbers differed by session.
- es2gears (Mesa WSI) does not use syncobj (Turnip-KGSL lacks DRM syncobj export) — not needed (same-process).
- Installed: `ld-20261026-d-explicit-sync`. Uncommitted: kgsl-syncobj.c/.so, config seed, session script, tools (fenceprobe.c, syncobjtest.c). Ask before committing.
- Next: Phase 1:2 zero-copy options 1–3 (hop 2), using the same timelines/fences idea for KWin → app.

## 2026-10-26 ~11:30: green video frames are a SEPARATE bug (not sync); explicit sync committed
- Committed: 014b1d1 (kgsl-syncobj emulation), ceec8be (fenceprobe, syncobjtest, cdp-eval.py, ld-shadetest VIDEO/URL/PROFILE_SRC + GREEN count). Not pushed.
- Wayland log (WAYLAND_DEBUG): Brave uses ONE surface, UBWC (QCOM_COMPRESSED) ABGR buffers, and EVERY commit has an acquire point → video is composited inside Brave; green is already in Brave's frame before KWin. Not fixable by Brave↔KWin sync.
- Green is decided **per Brave launch** (clean throughout, or a few/min up to several/s, 25–45% of the screen = the video). Ruled out by A/B: profile (my single-run profile bisect was misleading), variations seed, GPU shader caches, YouTube site storage, restored session, local VP9 & H.264 files. ProduceSkia "non-existent mailbox" error doesn't correlate.
- Running: `ld-videopool` (scripts/device) 6 launches each: D default / D --disable-gpu-memory-buffer-video-frames / C default. Results in ~/localdesktop-bench/videopool-full-*/summary.txt.
- 11:06 tombstone was Samsung's camera service (debuggerd dump), not ours; the app had been backgrounded/killed.

## 2026-10-26 ~12:20: Firefox FIXED (memfd reopen) + uBlock Origin; Firefox vs Brave green comparison running
- Firefox content processes died with "Shared memory PlatformHandle is not safe to map": Android SELinux forbids reopening a memfd via /proc/self/fd (EACCES, no audit log). Firefox's parent then falls back to unsealed POSIX shm while content processes (which skip that check) require F_SEAL_SHRINK → fatal. Firefox's own source says this "is always the case on Android".
- Fix: `memfd-reopen-shim.so` (guest/managed/usr/local/src/memfd-reopen-shim.c), session-wide: on EACCES for O_RDONLY open of /proc/self/fd/N, /proc/<pid>/fd/N or /dev/fd/N where N is a memfd → dup(N). Safe: Firefox seals frozen memfds with F_SEAL_FUTURE_WRITE.
- uBlock Origin via seed `/etc/firefox/policies/policies.json` (ExtensionSettings normal_installed from AMO). Needs its filter lists fetched once (fresh install stalls YouTube with "Experiencing interruptions?").
- APK `ld-20261026-e-firefox` installed (D + memfd shim + policies). Verified shell, Firefox (no crash, uBlock), Brave.
- Test template /root/greentest/ff-template (autoplay on, uBlock with fetched lists). `BROWSER=firefox ld-videopool`.
- Option A result: Brave's video frames reach its GPU process via shared memory (memfd), not dmabufs; Turnip-KGSL has no user-memory import → no CPU→GPU zero-copy race. Hypothesis A rejected; green is inside Brave's GPU compositing on Zink.
- Missing CJK fonts (Chinese shows boxes) — noto-fonts-cjk later.
- Uncommitted: memfd-reopen-shim, policies seed, session script, dmabuf-trace.c, ld-videopool firefox mode.

## 2026-10-26 ~16:20: PC crashed ~12:40; Brave had been BROKEN since build e — FIXED (build f), committed
- PC crash after 12:39. The last phone data before it: the frame stats froze at ~690 ms/frame (1.4 fps) at 12:38. The cause is unknown (logcat only goes back to 13:58). The phone itself did not reboot (uptime 7 days); the session came back fine.
- **The 12:21–12:30 "Firefox vs Brave 0/12 green (ok)" result was INVALID:** in all 6 Brave runs on `ld-20261026-e-firefox`, the GPU and network processes died at start (exit_code=5) and then Brave died (Trace/breakpoint trap). Nothing played. The last good Brave run was 11:35 (build C/D). The "Brave verified" check after installing e was not real.
- Cause: memfd-reopen-shim answered Brave's O_RDONLY reopen of /proc/self/fd/N with a **read-write** dup. Chromium's memfds have seals 0x7 (F_SEAL_SEAL, no write seal), so the fd stays writable forever; Chromium checks the fd mode and dies. Firefox reopens with seals=0 and adds F_SEAL_FUTURE_WRITE before sharing.
- Fix (aea55aa): the dup is only handed out while the memfd is write-sealed or still sealable (no F_SEAL_SEAL); otherwise EACCES stays and Chromium uses its own fallback. A/B on the phone: without the shim, Brave OK; old shim, Brave crashes; new shim, Brave OK and Firefox OK (no "not safe to map").
- ld-videopool now marks a launch **CRASHED** when the browser log shows a crash (c8f67ff). The 12:30 logs were checked: all 6 are flagged.
- Installed: **`ld-20261026-f-memfd-fix`**. Verified: build-id, shell screenshot (52 threads). Videopool (videopool-ff-vs-brave-1612): Brave 3/3 ok with 0 green, Firefox 3/3 ok with 0 green, ~66 redraws/s during play.
- Lesson: "Brave launch OK" means a launch whose log has no child deaths, not just a process that started.
- Unanalysed: glitch-rec-1236/ld-glitch.mp4 (12:36, 2 minutes). Branch perf/gpu-compositing-pacing is clean, not pushed.
- Next: green frames are still open (they are decided per launch, so 0/3 is not proof); then Phase 1:2 zero-copy.

## 2026-10-26 ~16:55: Firefox now on GPU (build g); Android shade stutter fixed
- User report: the Android pull-down shade lagged while Firefox played video; green flashes happen only in Brave.
- New tool `ld-shadejank <label> [cycles]` (BROWSER=firefox|brave|none): SystemUI gfxinfo jank and percentiles while pulling the shade, plus app CPU.
- Cause: clients had `LIBGL_ALWAYS_SOFTWARE=1` (the kopper-era route, restored for clients by localdesktop-plasma-mobile-shell). Mesa then reports a software device (`gfxtest glx -w`: MESA_ACCELERATED FALSE), so Firefox blocklisted the GPU: "WebRender (Software)", HW_COMPOSITING "blocked by platform", WebGL NO_DISPLAY. Firefox's Renderer+SwComposite threads used ~5 cores.
- The hardware EGL path now works (KWin GL offers linux-dmabuf on the fake renderD0): eglinfo shows every platform on Zink/Turnip except GBM; es2gears renders correctly without the variable and the kopper shim.
- Fix (4a76a5b): clients keep kwin-localdesktop-gl's environment (no LIBGL_ALWAYS_SOFTWARE). KWin on QPainter keeps the old route. Firefox about:support: Compositing "WebRender", WebGL on Turnip.
- Results (15 cycles, janky % / app CPU), f → g: Firefox 22.6% / 352% → 15.6% / 191%. Brave 20.3% / 221% → 15.4% / 168% (one g run was 26% because the user interrupted it). Idle 14.8% → 17.5% (noise). Android's shade janks ~15% even over an idle desktop.
- Installed: **`ld-20261026-g-hw-egl`**. videopool-g-1650: Brave 3/3 ok and Firefox 3/3 ok (no crashes).
- **The green detector is not reliable:** during g-brave-1 the user saw green flashes, but frame-stats GREEN total = 0. On build f, shadejank-brave-1 counted 40. "0 green" from ld-videopool or ld-greentest is no proof; check the detector (probe positions and thresholds) against what the user sees before using it to judge green fixes.
- Branch is clean at 373ce91, not pushed.

## PAUSED 2026-10-26 ~17:00. Resume here (next session 2026-10-27).
### State
- Phone: **`ld-20261026-g-hw-egl`** installed (APK in ~/localdesktop-bench/apks/). It has: explicit sync (kgsl-syncobj), memfd-reopen-shim with the seal rule, hardware EGL for clients, and Firefox + uBlock policy. Verified: shell, Brave, Firefox (hardware WebRender), no crashes.
- Repo: branch `perf/gpu-compositing-pacing`, clean at 373ce91, **not pushed**. Today's commits: 5e92ae9 … 373ce91 (explicit sync, device tools, Firefox memfd fix, hardware EGL, ld-videopool CRASHED state, ld-shadejank).
- Resume check: `adb shell dumpsys power` (awake?), app in the foreground, `ld-run 'cat /etc/localdesktop/build-id'`, screenshot with `screencap -p -d 4630947175568309891`.
### Next, in order
1. **Fix the green detector** (frame-stats GREEN in the app's compositor). Today it counted 0 while the user saw green in Brave (build g, shadejank-g-brave-1). Find where the probes sample and with which thresholds (R,B<16, G>=48?), and whether the flashes are a different colour, partial, or between probes. Validate it against a recording: glitch-rec-1236/ld-glitch.mp4 (12:36, never analysed; pts.txt has frame times) or a fresh `screenrecord` while the user watches Brave. Pass = the detector fires whenever the user sees green.
2. **Green frames (Brave only; Firefox never shows them).** Known so far: decided per launch; the green is already in Brave's frame before KWin; video is composited inside Brave on Zink; frames reach the GPU process via shared memory. Note: Brave now also runs on the hardware EGL path (build g) — re-baseline before comparing with older runs.
3. Phase 1:2 zero-copy (app upload ~3 ms/frame).
### Open items / caveats
- PC crash ~12:40: cause unknown. The phone was fine (uptime 7 days). The frame stats at 12:38 showed ~690 ms/frame just before; logcat didn't reach back that far.
- Memory pressure on the phone: during Firefox playback 370 MB free and 5.4 GB swap used. Watch for it in perf tests.
- Android's shade janks ~15% even over the idle desktop (SystemUI baseline here); not investigated.
- zink-kopper-shim and LIBGL_ALWAYS_SOFTWARE now only matter for KWin on QPainter (/etc/localdesktop/kwin-qpainter). They are still preloaded, harmlessly. Candidate for removal once QPainter is dropped.
- The auto-mode classifier blocks Brave launches with `--no-sandbox`; diagnose by other means (preload logging worked today).
- Lessons: after any install, check the shell screenshot AND a browser launch whose log has no child deaths ("exited unexpectedly", "Trace/breakpoint trap"). A "0 green" result means nothing unless the run is ok AND the detector has been validated.
- Still open from before: DNS only refreshed at app start; CJK fonts missing (noto-fonts-cjk); the guest overlay never deletes files dropped from the APK.

## 2026-10-27 morning: session killed by Android's phantom process killer; green detector fixed; green = Android overlays
- **10:02 session death** (screen frozen on the video, the runner spinning at 100% CPU as an orphan): ActivityManager "Killing PhantomProcessRecord … Trimming phantom processes" killed kwin_wayland, libproot, pipewire, dbus at once (Brave + screenrecord pushed the app over 32 child processes). Fix: the user turned on Developer options → **"Disable child process restrictions"** (stored as `persist.sys.fflag.override.settings_enable_monitor_phantom_procs=false`; `settings get global …` stays null). Upstream docs already require it (getting-started "Disable phantom process killer"). Yesterday's 12:38 freeze was probably the same thing (unproven; logs gone).
- Follow-up (app robustness, not done): when Android kills the guest, the app keeps showing the last frame and leaves orphaned guest processes spinning. It should detect proot's death, kill the rest, and say so.
- **Green detector fixed (b20eb26, build `ld-20261027-h-green-detector`, installed):** a recording showed the green frames are the right picture in green only = **chroma reads as zero** (luma correct). The old rule (G 48–180, ≥25% of the grid) missed bright scenes and non-fullscreen video. New rule: R,B<16, G≥48, ≥10% of the grid, and not all saturated (to skip synctest). Validated with `ld-greenrec` (screenrecord + offline analysis): grayscale 10/10 events; YouTube in a page 8 frames (5 below the old threshold); 3 clean runs with 0 detections. YouTube artifacts can be partial: vertical stripes of green = a partly written chroma plane.
- **Green is triggered by Android overlays:** every recorded event fell while an overlay covered our window (a "Copied." toast from Samsung clipboard activity at 10:15:27–46; a message heads-up notification). Reproducible test: `VIDEO=1 URL=file:///root/greentest/index.html ld-shadetest <label> 12 "--start-fullscreen --allow-file-access-from-files"` → 55 / 28 / 38 GREEN frames in 3 launches; no-overlay control (`SHELL_ENV=1 ld-greentest <label> 60`) → 0 / 0. **This is the pass/fail test for the green fix.**
- Next: find what an Android overlay changes for our app (focus/visibility callbacks, SurfaceFlinger composition, frame pacing toward KWin), then why that exposes Brave's chroma race (Zink upload of the UV plane vs. sampling in Brave's GPU process).
- 11:10 lead, **WITHDRAWN 11:30**: `guest/tools/xctxtest.c` (upload in GL context A, sample in B) failed a few times per 2000 under `guest/tools/gpuload.c` (finish 2+5, flush 0+3; ZINK_DEBUG=noreorder / GALLIUM_THREAD=0 / sync gave 0). It did **not reproduce later** (0/2000 everywhere, both baselines; ~48 °C), so there's no confirmed Zink bug. Maybe a leftover Brave was running then.

## 2026-10-27 ~11:35: green is Chromium-specific (driver stack cleared)
- Brave's stack: Skia GaneshGL → ANGLE (GL backend) → Zink → Turnip; VA-API fails, so decode is software.
- Shade test (12 pulls over the grayscale video, GREEN frames), Brave: default 55/28/38; ZINK_DEBUG=noreorder 40/33/38; GALLIUM_THREAD=0 48/27; **ZINK_DEBUG=sync 44/36** (fully synchronous Zink, still green); **--use-angle=vulkan 40** (chrome://gpu confirms ANGLE_VULKAN directly on Turnip, no Zink: still green). The ZINK_DEBUG env does reach Brave's GPU process (ZINK_DEBUG=nir printed shaders; /proc/environ of Chromium children is clobbered by its setproctitle, so don't trust it).
- **Firefox, same video, same 12 shade pulls: 0 GREEN frames.**
- Turnip/KGSL: `guest/tools/vkfencetest.c` (32 MiB GPU copy, fence or queue-idle wait, CPU check) 0/200 bad with and without gpuload. KGSL ignores the user's preemption style (the kernel rewrites the flags 0x13 → 0x8052; `guest/tools/kgsl-ctxflags.c`).
- ⇒ The cause is in Chromium's video-frame path above the GL backend (software-decoded frames → shared memory → GPU process upload/compositing), disturbed by Android overlays (CPU/GPU scheduling). Green = chroma plane zero or partly written (luma correct).
- Next ideas: (1) the shared-memory path for video frames: which region type Chromium falls back to after memfd read-only reopen fails (EACCES), and whether the GPU process can read a frame before the decoder finished writing it; (2) Chromium flags as diagnostics only: e.g. disabling zero-copy/one-copy video upload paths, `--disable-features=...` for VideoFrame shared-image paths, `--single-process`-like isolation; (3) Chromium trace (chrome://tracing / --trace-startup) around a shade pull.
- Tools added: ld-shadetest `ENV=` (extra env for Brave), cdp-eval.py URL filter (3rd arg), and to open chrome:// pages use `PUT http://127.0.0.1:PORT/json/new?chrome://gpu`.

## 2026-10-27 ~12:55: app GPU-mapping leak FIXED (build i installed)
- 12:15 the app itself aborted (not the phantom killer): Adreno-GSL "sharedmem_gpumem_alloc: mmap failed errno 12" ×36,000, then mremap/malloc ENOMEM → SIGABRT. The app had hit the per-process limit on memory mappings (~65,530).
- Cause (located with a temporary per-phase counter of /kgsl-3d0 mappings): smithay's Android `EGLContext::make_current` created a new 1×1 pbuffer on every call and never destroyed it; `import_dmabuf_by_copy` calls make_current once per client frame → +29 mappings/s during video → limit after ~36 min of video. The 10-26 12:38 freeze before the PC crash was likely this too.
- Fix 969316f (patches/smithay/src/backend/egl/context.rs): one pbuffer per context (OnceLock), destroyed in Drop. Build `ld-20261027-i-pbuffer-leak` installed: mappings flat at 56 KGSL / 2793 total over 80 s of video (were +29/s). Shell OK.
- A first hypothesis (re-mmapping the dmabuf every frame) was tested and rejected; that change was reverted.

## 2026-10-27 ~13:30: green traced to Chromium's MappableSharedImage video-frame pool (shared memory)
- `--disable-gpu-compositing` is unusable here (black output, ~2 buffers/s), so it doesn't count.
- **`--disable-gpu-memory-buffer-video-frames`: 0 green in 2 launches (video playing 26–30 fps) vs default 45** (build i, shade test). The old "nogmb makes no difference" result came from the broken detector — void.
- Chromium 155 source (gitiles tag 155.0.8059.40; fetch with retries, gitiles 504s): media/video/mappable_shared_image_video_frame_pool.cc. Software-decoded frames are copied by worker tasks (CopyRowsToNV12Buffer, row chunks covering Y+UV together, BarrierClosure → OnCopiesDone) into a pooled mappable SharedImage, then `BackingWasExternallyUpdated(sync_token)` and a VideoFrame with read_lock_fences_enabled. Live chrome://histograms: `Media.GPU.OutputFormatSoftwareGmb` 561 samples → the buffers are **SHARED_MEMORY_BUFFER** (GPU process uploads shm → textures on access), not dmabufs.
- Grayscale green = full luma, zero chroma ⇒ more like the UV plane's GPU upload missing than an unfinished CPU copy (a CPU chunk writes both planes). YouTube artifacts = partial chroma (vertical stripes).
- Next: Chromium trace (--trace-startup with media,gpu,viz categories) around a shade pull: CopyVideoFrameToMappableSharedImage vs the GPU-side compound/shared-memory backing upload per plane; or read gpu/command_buffer/service/shared_image/compound_image_backing.cc + shared_memory_image_backing for per-plane upload/skip logic. A per-app flag would be a workaround (the user rejects those).
- ~13:45: in a default shade run with 35 GREEN frames, chrome://histograms shows **GPU.CompoundImageBacking.ContentSync.Success 1766/1766 true**, source kSharedMemory (10) → dest **kGLTexture (7)** (ANGLE texture via UploadFromMemory), TotalElementsAllocated 10 samples (10 pool images created), no "Failed to copy" in the log. So Chromium believes every shm→texture upload succeeded: the UV bytes were zero in shared memory when uploaded (or became zero). The renderer copy (CopyRowsToNV12Buffer) writes Y and UV rows together per chunk (an empty plane span skips both), so a partial CPU copy doesn't explain full luma + zero chroma.
- Open hypotheses: (a) the GPU-side copy reads a reused or fresh pool buffer at the wrong time (sync-token ordering between renderer and viz sequences in the GPU process); (b) something zeroes shm pages under memory pressure (overlays raise Android memory pressure; Chromium's memory-pressure handling / MADV_* on memfd); (c) the decoder's source frame chroma. Tool to decide: a Chromium trace (`--trace-startup=media,gpu,viz,disabled-by-default-gpu.service --trace-startup-file=... --trace-startup-duration=N`) around shade pulls, plus a test of (b) by applying memory pressure without an overlay.

## 2026-10-27 ~13:25: green flashes worked around (build j, d4a1104); root cause still open
- Installed: **`ld-20261027-j-brave-video-pool`** = build i (pbuffer leak fix) + `BRAVE_FLAGS=--disable-gpu-memory-buffer-video-frames` in the session (startplasmamobile-localdesktop, GPU block) + the same flag in the generated /usr/local/bin/chromium wrapper (src/android/proot/setup.rs).
- Why BRAVE_FLAGS: /usr/bin/brave-origin-nightly (AUR) passes "$USER_FLAGS" (from ~/.config/brave-origin-nightly-flags.conf, which holds `--no-sandbox`) as ONE argument, and "$BRAVE_FLAGS" as another. A second line in flags.conf would break both.
- Verified: shade test 0 / 0 green with the flag, **36 with `ENV=BRAVE_FLAGS=`** (the bug is still there underneath); video 26–29 fps; YouTube videopool 2/2 ok, 0 green; Brave's cmdline carries the flag; shell OK.
- **To reproduce the bug now:** `ENV=BRAVE_FLAGS= VIDEO=1 URL=file:///root/greentest/index.html ld-shadetest <label> 12 "--start-fullscreen --allow-file-access-from-files"`.
- Not covered: Chromium launched from the app drawer (its desktop entries only get --no-sandbox; Chromium isn't installed); Electron apps.
- Cost of the flag: frames skip the GPU-memory-buffer copy and go to the compositor as software frames (the regular VideoResourceUpdater upload path). No playback difference seen at 1080p; not measured for CPU.

### TO REVISIT: root cause of the zero-chroma frames (the flag is a workaround)
Known: luma right, chroma zero (grayscale) or partly written in vertical stripes (YouTube); shared-memory MappableSI pool (Media.GPU.OutputFormatSoftwareGmb); every shm→kGLTexture content sync reports success (GPU.CompoundImageBacking.ContentSync.Success 1766/1766); the renderer copy writes Y and UV rows together per chunk; triggered by Android overlays (and weakly by other GPU load); Firefox unaffected.

**Test 1 — memory pressure (cheap, ~15 min).** Hypothesis: overlays raise Android memory pressure; Chromium (or the kernel, via its memory-pressure handling) drops or zeroes shared-memory pages still in use (e.g. MADV_FREE/MADV_DONTNEED/punch-hole on memfd), so the GPU process uploads zeros.
1. Play the grayscale video in Brave with the bug enabled (`ENV=BRAVE_FLAGS=`, or `SHELL_ENV=1 ld-greentest` with BRAVE_FLAGS cleared) and NO overlay; baseline is 0 green (h-nooverlay-1/2).
2. Apply memory pressure without an overlay, e.g. `adb shell am send-trim-memory app.polarbear RUNNING_CRITICAL` (app-level), plus system-level: a guest allocator that maps and touches ~3–4 GB (python3 -c "b=bytearray(3<<30)") for 20 s, or Android's `am memory-factor set CRITICAL` if available on Android 16.
3. Watch Chromium's view: chrome://memory-internals / `Memory.PressureLevel` in chrome://histograms; and the PSI file Chromium reads (/proc/pressure/memory inside the guest).
4. Green during pressure ⇒ follow it: strace isn't available under proot, so use an LD_PRELOAD that logs madvise/fallocate/ftruncate on memfds in the renderer and GPU process (pattern: guest/tools/dmabuf-trace.c).
5. No green ⇒ memory pressure is ruled out; go to test 2.

**Test 2 — Chromium trace (an hour or more).** Goal: order of the renderer's copy and the GPU-side upload for the frames that come out green.
1. Launch Brave (bug enabled) with `--trace-startup=media,gpu,viz,gpu.service,disabled-by-default-gpu.service,disabled-by-default-viz.gpu_composite_time --trace-startup-file=/tmp/brave-trace.json --trace-startup-duration=40` plus the grayscale page; pull the shade ~6 times between 15 s and 35 s; collect the green frame times from logcat (frame-stats GREEN; convert to CLOCK_MONOTONIC, which the trace uses: log `date +%s.%N` and `cat /proc/uptime` together at the start).
2. In the trace, per pool buffer (SharedImage mailbox): `CopyVideoFrameToMappableSharedImage` begin/end and `CopyRowsToNV12Buffer` slices (renderer, worker threads) vs the GPU process's CompoundImageBacking content sync / UploadFromMemory for the same mailbox, and the viz draw that sampled it.
3. Look for: a GPU-side copy before the renderer's copies ended (sync-token ordering between the renderer and viz sequences); a pool buffer handed out again while still being read (read-lock fences); new pool allocations (GPU.CompoundImageBacking.TotalElementsAllocated) coinciding with green.
4. Sources (Chromium 155.0.8059.40 = Brave's; gitiles gives 504s, so fetch with retries `?format=TEXT` + base64): media/video/mappable_shared_image_video_frame_pool.cc, gpu/command_buffer/service/shared_image/compound_image_backing.cc, shared_memory_image_backing.cc, gl_texture_image_backing.cc (UploadFromMemory), cc/resources/video_resource_updater.cc. Copies are in the scratchpad of the 10-27 session (not kept).
5. If it's a Chromium bug: file it upstream (Chromium + Brave) with the grayscale repro, the overlay trigger, the histograms above, and the trace; then drop BRAVE_FLAGS once fixed.

### Other follow-ups from 10-27
- When Android kills the guest (phantom killer or otherwise), the app keeps showing the last frame and leaves orphaned guest processes (a runner `sh` spun at 100% CPU). It should detect proot's death, kill the leftovers, and show a message or restart.
- `xctxtest` failed a few times early in the day under gpuload and never again; not a confirmed bug. Re-run it if GL glitches appear: `guest/tools/xctxtest.c` + `gpuload.c`.

## 2026-10-27 ~14:45: Phase 1:2 zero-copy WORKS (build k installed, uncommitted)
- The user chose zero-copy as the next step (14:10). The green root-cause tests stay parked under "TO REVISIT". I had started Test 1 by mistake; it was stopped and nothing was left running (memory-factor reset).
- **Option 1 was the right path, by a route not in the notes:** the stock Adreno driver (GL `GL_EXT_memory_object_fd`, Vulkan `VK_KHR_external_memory_fd`) exports "opaque" fds that are ordinary `system` dma-heap dmabufs (the same heap Turnip uses), with a **5300-byte metadata block in the last 8 KiB**. It refuses plain dmabufs (Vulkan INVALID_EXTERNAL_HANDLE; GL TexStorageMem2D GL_INVALID_OPERATION) but imports any dma-heap dmabuf whose end carries that block, and really shares the pages (CPU rewrites show without a re-import).
  - Block layout (offsets from the start of the last 8 KiB): 2892=1, 2896=pid (the only field that changes between exports), 2900=data+5300, 2908=0x40, 2912=format (0x57 BGRA8, 0x1c RGBA8, 0x3e for buffers), 2916/2920/2928/2932=1, 2940/2960/2972=data size, 2948=1, 2968=row pitch, 2980=bpp, 2984/2988=w/h, 2992/2996=aligned w (=pitch/4)/h, 8152=0x5143 ("QC"), 8156=6, 8160=0x0060c510, 8164=0xc5100014, 8168=4, 8172/8176=1, 8184=5300, 8188=0xde5cda7a. The driver honours the pitch we write: Turnip's 7424 for 1812 px (Adreno itself would use 7296). Slack between the image and the block is fine.
  - Probes (NDK, run via adb shell): `tools/android/{memobjprobe,vkmemprobe,vkimgtrailer,gltrailerprobe}.c`, `tools/android/build.sh <name>`; guest `guest/tools/tulinear.c` (Turnip's linear layout). gltrailerprobe A = image import PASS, 0.55 ms/frame; mode B (GL buffer import) fails, and deleting the memory object breaks the texture (it must live as long as the texture).
- **Implementation (build `ld-20261027-k-dmabuf-zerocopy`):**
  - kgsl-drm-shim pads every `DMA_HEAP_IOCTL_ALLOC` to a page multiple + 8 KiB (KWin's buffers are now 15974400 bytes, previously 15966208).
  - smithay `GlesRenderer::import_dmabuf_shared` (patches/smithay/.../gles/mod.rs) runs before the copy path for linear single-plane ARGB/XRGB/ABGR/XBGR at offset 0 with ≥8 KiB slack. It writes the block (`write_adreno_metadata`), imports through a dedicated memory object plus `TexStorageMem2DEXT` (linear tiling), and caches the texture per dmabuf. The memory object is freed with the texture (`CleanupResource::MemoryObject`). On a GL error it logs a warning and switches to the copy path for good (protection against driver updates). The GL bindings gained GL_EXT_memory_object(_fd). The green probe still runs (`probe_dmabuf`).
  - A/B switch: `run-as app.polarbear touch files/dmabuf-copy` before the app starts keeps the CPU copies. Log line at start: "dmabufs: zero-copy import through GL_EXT_memory_object_fd enabled".
- **Results (same build, switch on vs off, ld-framebench ×2, ~/localdesktop-bench/ab-k/):** gears render 5.25 → 0.83 ms; app CPU in gears 690 → 247 cs (−64%); drawer app 106–172 → 52–70 cs; fps unchanged (gears 77–84 vs 78–83); KWin in gears ~416 → ~460 cs (+10%, not investigated).
- **Checks:** shell screenshot OK; synctest 845 frames, 0 mixed at commit time; screen recording during synctest 357/357 frames, 0 mixed; Brave grayscale video (SHELL_ENV=1 ld-greentest k-zc-brave 60) 28.6 buffers/s, 0 green, 0 child crashes, picture correct.
- **Open risks / next:**
  1. ~~Release race~~ **tested 15:05, clean**: synctest at 50 shader iterations (fast frames), 30 s each, screen recorded 20 s, every recorded frame checked (scratchpad `racetest` + `mixcheck.py`, streaming; the first version was OOM-killed on 1.7 GB of frames and printed nothing): fast 70 fps 2394 frames 0 mixed; + `/tmp/gpuload 35 200 10` 42 fps 2398 frames 0 mixed; + 6 shade pulls 52 fps 2392 frames 0 mixed (81 covered by the shade). Commit-time counters were 0 mixed in all three. The detector fires on a synthetic half-red/half-green frame. There was no known-bad recording to validate it against.
  2. KWin's +10% in gears.
  3. The format is reverse-engineered from driver 0676.77; a driver update falls back to copies, with a warning in logcat.
  4. **Committed 1d3ba03** on perf/gpu-compositing-pacing (not pushed). The installed build k matches it (only a probe comment changed after the build).

### Future improvements: zero-copy options 2 and 3 (not built; the user asked to note them, 10-27)
Option 1 (above) leaves ~0.83 ms of app render per frame, which is the draw itself, so 2 and 3 can't be much faster. Their value is **robustness**: option 1 depends on Adreno's undocumented metadata block (driver 0676.77). A driver update that changes it drops the app back to CPU copies, with the logcat warning "dmabufs: zero-copy import failed ... using copies". Build them if that happens, or to get the planned measured comparison. Compare with `ld-framebench` against build k (zero-copy) and build k with `files/dmabuf-copy` (copy), plus racetest/mixcheck for tearing.
- **Option 2: Android Turnip inside the app** (adrenotools-style loading; reference: DroidDeck `app/src/main/cpp/waylandcomp/src/vk_present.c`, `ahb_swapchain.c`, `wl_dmabuf.c`; clone at ~/src/DroidDeck). Turnip has `VK_EXT_external_memory_dma_buf`, so it imports KWin's dmabufs without metadata tricks.
  - 2a: Turnip copies each client buffer on the GPU into an AHardwareBuffer, which GL samples through `EGL_ANDROID_get_native_client_buffer`. Still a copy (GPU-side), so it costs more than option 1. It needs cross-driver sync (sync_file between Turnip and Adreno GL in one process).
  - 2b: a Vulkan presenter on Turnip replaces smithay's GLES renderer (ANativeWindow swapchain). True zero-copy on an open driver, but it rewrites the app's render path.
  - Risks: two GPU drivers in one process; Turnip-on-KGSL on Android's kernel 5.15 inside the app's SELinux domain.
- **Option 3: host-allocated AHardwareBuffers lent to the guest.** The app allocates gralloc buffers (documented NDK API), sends their dmabuf fds to KWin (`AHardwareBuffer_sendHandleToUnixSocket` or the native handle), and samples them with `EGL_ANDROID_get_native_client_buffer` (documented, zero-copy). Guest side: Turnip imports the dmabuf (KGSL import, as for any dmabuf), and KWin renders into buffers the host provides instead of allocating its own. That needs a custom Wayland protocol (or a GBM/allocator shim in the guest that takes buffers from the app) plus knowing gralloc's layout (pitch/tiling) for linear RGBA.
  - Risks: KWin's nested-Wayland backend allocates through GBM, so a KWin patch or GBM shim is likely; buffer count and lifetime have to be negotiated.

## 2026-10-27 ~15:45: STABLE working copy (tag `stable-20261027`)
- Commits: 1d3ba03 (zero-copy), df374a4 (ld-racetest), then an ld-racetest fix (`-fps_mode passthrough`: screenrecord wrote a 90000/s rate into one recording's header, and ffmpeg padded it to ~1.8 M frames, so the check ran for hours). Branch perf/gpu-compositing-pacing, not pushed, tree clean.
- Installed: **`ld-20261027-l-stable`**, built from the clean tree at df374a4 (the later commit only touches a PC-side script). Archive: `~/localdesktop-bench/stable-20261027-l/` (APK + SHA256SUMS + commit.txt + framebench and race results + Brave green log).
- Verified on this build: ld-abrun (build ID, shell screenshot, 52 shell threads, framebench ×2: render ~0.84 ms, 0 mixed/green); Brave grayscale video 60 s with SHELL_ENV=1: 28.4 buffers/s, 0 green, 0 child crashes; ld-racetest: synctest 74.5 fps, 1503/1503 recorded frames, 0 mixed.
- The first gears window was 57/63 fps (second window 84). Within earlier variance (10-26 builds show 53–67 in some first windows); the app's frame-stats show a ramp-up from ~49 fps in the first seconds.
- Next project: not chosen. Candidates: green root cause ("TO REVISIT"), Steam + FEX, guest-death handling, KWin +10% CPU since zero-copy, zero-copy options 2/3 (robustness).

## 2026-10-27 ~16:00: Steam + FEX started (from DroidDeck's recipes) — IN PROGRESS
- DroidDeck clone updated to 4f58230a (~/src/DroidDeck). Relevant: docs/development/arm64-steam-client.md, x86-64-steam-client.md, game-environment.md; tools/linuxfs/overlay/usr/local/bin/{droiddeck-steam-install,droiddeck-session,steam-compatibility,droiddeck-fex-update}; tools/fex (FEX built on Arch Linux ARM, release fex-2610-r5 in Droid-Deck/DroidDeck-Components, sha256-pinned); tools/linuxfs/preload/*.c (one combined session library; taken only as failures show up).
- Their default path: Valve's **arm64** Linux client (`steamrtarm64/steam`, channel steamdeck_publicbeta, manifest steam_client_steamdeck_publicbeta_linuxarm64) plus Valve's "Proton Experimental/11.0 (ARM64)" (app 4427310, downloaded by the client). FEX runs inside Proton (arm64ec). The x86-64 client under FEX is their opt-in (VAC).
- Done on the phone:
  - `droiddeck-steam-install` (unchanged, copied to /root) installed the client into /root/.local/share/Steam. The client self-updated (670 MB) and then exited 42 (restart request).
  - Scratch launcher /root/ld-steam (scratchpad copy): STEAM_RUNTIME=1, STEAM_STEAMRT_RECURSION_GUARD=1, LD_LIBRARY_PATH=steamrtarm64:steamrtarm64/panorama, `-clientbeta <package/beta> -no-cef-sandbox -cef-ozone-platform=x11`, restarts on rc 42 (up to 5×). Started with plasmashell's environment (DISPLAY=:0 Xwayland).
  - Installed `openal` and `libibus` 1.5.34-2 (pacman -U; the guest's sync DBs date from 10-01, so ld-pacman fails silently on 404s: its `set -e` plus xargs curl. Fetch current files by hand).
- Missing for the client: **GTK 2** (`steamui.so`, `vgui2_s.so` need libgtk-x11-2.0; Arch moved gtk2 to the AUR, and ALARM has no build). Steam's bundled FFmpeg (`libav*.so`) needs soname links (libavutil.so.59 etc.), which the launcher should make.
- gtk2 build prepared: `guest/pkgbuilds/gtk2/PKGBUILD` (AUR 2.24.33-5 for aarch64, from the release tarball, sha256 ac2ac757…), `guest/pkgbuilds/build.sh` (glib2-devel 2.90 tools unpacked beside the build; the guest has glib2 2.88, and the mirror now has 2.90). **Blocked:** makepkg refuses to run as root, everything is root under proot, and the auto-mode permission check denied running a copy of makepkg with that check removed. Waiting for the user's decision.
- **16:35 Steam sign-in window shows** (arm64 client, X11 via Xwayland, CEF on ANGLE/Vulkan). Took:
  - gtk2 built from guest/pkgbuilds/gtk2 (pacman package gtk2 2.24.33-5). build.sh uses a temporary makepkg copy without the root check, deleted after the build (the user approved it as temporary);
  - lsof plus the loopback-owner fix: Steam asks `/usr/sbin/lsof -P -F upnR -i TCP@127.0.0.1:PORT` who is on the other end of each loopback connection, and refuses the UI websocket otherwise. Android hides /proc/net/tcp, so the real lsof answers nothing. New: `loopback-owner.so` (session preload: records local port → pid/ppid/uid/peer/fd/inode in /tmp/localdesktop-loopback/ on connect/accept), `/usr/local/bin/lsof` (answers from the records after checking that the fd is still that socket; else runs the real lsof), `/usr/local/libexec/localdesktop-lsof-divert` + pacman hook `localdesktop-lsof.hook` (/usr/bin/lsof → the wrapper, real binary in /usr/lib/localdesktop/lsof.real; /usr/sbin is /usr/bin on Arch), and the session start runs the divert. Idea from DroidDeck net.c, which does it inside exec hooks.
  - The running session doesn't have the preload yet (Steam was started with LD_PRELOAD extended by hand); it needs an APK build.
- Remaining Steam warnings: no NetworkManager ("failed to create a NMClient"; DroidDeck runs a stand-in, droiddeck-netmanager), CM "Try another CM" after ~1 min while not signed in, GPU process gbm export errors (EINVAL), XRRGetOutputInfo, udev enumerate.

## 2026-10-28 ~09:20: Steam sign-in, no on-screen keyboard
- Cause: Steam runs as an X11 client (Xwayland), so KWin never gets a text-input focus and plasma-keyboard never auto-shows. Every Xwayland app is affected.
- Tried `-cef-ozone-platform=wayland` (libcef 126 has ozone Wayland + zwp_text_input_manager_v1): the main `steam` binary still needs DISPLAY ("Unable to open a connection to X"), and with DISPLAY=:0 the webhelper restarts in a loop (44×), dying right after `CBrowserComposerSystem::CreateOutputWindow` (Valve's own output window is X11-only). **Wayland for Steam is not viable**; reverted to the stock x11 launcher.
- Steam build 1793056502 (2026-10-27) is current.
- Side issue: the "bad symlink steamrtarm64/libavresample.so.4" verification failure made the client re-extract its package on the restart (localdesktop-steam's soname links).
- Plasma Mobile's nav bar has a keyboard button (bottom right). Next: check that it shows plasma-keyboard over Steam and that KWin delivers the typed keys to the X11 window.
- **Fixed (09:23):** localdesktop-steam overwrote Steam's real `libavresample.so.4` with a soname symlink on every launch → verification failure → full package re-extract (~80 s) + restart on each start. Now links only missing sonames. Copied into the guest; uncommitted in guest/managed (Steam files are still untracked).
- Launching Steam from ld-run's environment leaves steamwebhelper defunct; launch with plasmashell's environment (`xargs -0 -a /proc/$P/environ ... env -i`). The sign-in window is back (09:26). Steam logs "XOpenIM() failed" (no XIM server), a possible future route for X11 IME.
- **09:4x: user signed into Steam** (typed with plasma-keyboard via the nav-bar keyboard button; the first try failed with a wrong-password error, cause not confirmed). Next step not chosen yet.

## 2026-10-28 ~09:55: first game (Bastion, 107100) via FEX
- Bastion installed (Linux depot 107106, 1.54 GB): FNA/Mono, Bastion.bin.x86_64 (+x86), bundled SDL2/FMOD/mojoshader/mono in lib64; OpenGL.
- FEX fex-2610-r5 installed in /usr/local (DroidDeck's sha256-pinned build; installer run via ld-run for network). `localdesktop-fex status`: FEX + FEXServer + thunks found.
- New in guest/managed/usr/local/bin (uncommitted): `localdesktop-fex` (= DroidDeck 4f58230a droiddeck-fex, only state paths/updater name changed; state in ~/.local/share/localdesktop-fex), `localdesktop-fex-update` (= droiddeck-fex-update). `localdesktop-steam` now writes compatibilitytools.d/localdesktop-fex ("Linux x86 (FEX)") at each start (Steam reads it only at startup).
- Runtimes: SLR 4.0 (4183110) installed; sniper (1628350, 772.8 MB) install dialog shown to the user. Scout (1070560) installed by Steam itself.
- Next: Steam restart (to see the tool) → Bastion Properties → Compatibility → "Linux x86 (FEX)" → launch; then debug.
- **~10:10 Bastion RUNS under FEX** (direct launch, outside Steam's Play button): auto-save splash rendered fullscreen; FNA reports OpenGL 4.6 zink / Turnip Adreno 740 via FEX GL thunks; FMOD initialised. FEX log noise: "STOSOp address size override", "GS selector in 64bit mode", "Missing LOCK HANDLER CMP", clone3 CLONE_CLEAR_SIGHAND EINVAL (not yet known to matter).
- **Root cause fixed:** DroidDeck's build_rootfs links rootfs entries absolutely into Steam's runtime folders; FEX opens guest paths with openat2 RESOLVE_IN_ROOT (DroidDeck's patch emulates it under PRoot, which returns ENOSYS), so absolute links resolve *inside* the rootfs → "libc.so.6: cannot open". Verified by copying libc only (x86 uname then ran). localdesktop-fex now copies the runtime (ROOTFS_LAYOUT 3, copy_in, dangling links skipped): 792 MB, 30 s, once per runtime version. Only ~/.steam/sdk{32,64} links stay absolute (FEX's host-path fallback covers them).
- Test command: ld-run with plasmashell env + SteamAppId=107100, `localdesktop-fex run -- ./Bastion.bin.x86_64` in common/Bastion/Linux; log /tmp/bastion-run.log.
- Next: Steam restart → Properties → Compatibility → "Linux x86 (FEX)" → Play from Steam.
- **~10:30 Steam Play launch failed → two more fixes:**
  1. **proot patch 0019** (`patches/build-proot-android/patches/0019-localdesktop-execveat-absolute-path.patch`, README updated): proot returned ENOSYS for execveat with a non-AT_FDCWD fd; FEX (no binfmt_misc) execs every x86 child as `execveat(<loader fd>, "/proc/self/exe")`. Now absolute path + no AT_EMPTY_PATH → execve, as the kernel does. Test: `localdesktop-fex run --mode on -- $R/bin/sh -c "uname -m"` → stock proot "aarch64" + "proot error: execveat() ... not supported"; patched "x86_64". DroidDeck does this in a preload (robust.c).
  2. **localdesktop-fex guest PATH**: /usr/bin:/bin first (the session's sbin-first PATH fell through to host /usr/sbin = /usr/bin ARM binaries).
- APK **ld-20261028-a-fex** built (241 s) and installed; archived `~/localdesktop-bench/apks/ld-20261028-a-fex.apk`. Bastion via its launcher script → x86_64 binary, GL zink/Turnip. All uncommitted.
- **~10:35 MILESTONE: user launched Bastion from Steam (Play, "Linux x86 (FEX)") → title screen, no crash.** Gameplay untested: no controller. Bastion's PC controls need keyboard (+mouse).
- Open: input on a phone (touch → mouse under Xwayland? plasma-keyboard keys reach X11 apps, seen at Steam sign-in; Bluetooth controller/keyboard), FEX log noise, commit everything (all uncommitted: proot 0019, localdesktop-fex*, localdesktop-steam changes, Steam/lsof files from 10-27).

## 2026-10-28 ~11:30: emulated Steam Deck controller (steps 1–2, PC side) — design in docs/steam-deck-controller.md
- Committed earlier: 7176070 (proot 0019), ba85d4d (Steam), 64fa747 (FEX).
- New (uncommitted): guest/managed/usr/local/src/fakeinput_steam.cpp (DroidDeck libfakeinput, unchanged), guest/build-fakeinput.sh (Debian bullseye via docker, source on stdin / tar out — bind mounts fail under Fedora SELinux), built libs usr/local/lib/libfakeinput.so (aarch64) + usr/local/lib/localdesktop-fex/x86_64/libfakeinput.so; guest tool localdesktop-pad (setup/env/press/release/tap/stick/trigger/dpad/state); session start runs `localdesktop-pad setup`; localdesktop-steam preloads the lib + env; app binds $ARCH/sys/.pad/{devices,hidraw} over /sys/devices/droiddeck and /sys/class/hidraw (process.rs pad_sysfs).
- PC end-to-end test passed (x86-64 build in Debian; the lib serves hidraw16 only to an exe named `steam`).
- APK ld-20261028-b-pad building (not installed). Next on the phone: install, check Steam lists a Deck controller, press buttons via `ld-run 'localdesktop-pad tap A'`.
- **~11:32 Deck controller WORKS in Steam (commit 49eaf89, build ld-20261028-c-pad installed + archived):** handshake 0xAE/0x87/0x8F/0x81, Deck config set loaded; Big Picture: d-pad right moved the selection, Steam button opened the menu, B closed it. Fixes on the way: FAKE_DECK_SYSFS_LISTING stand-ins (Android refuses listing /sys, /sys/class, /sys/bus); pad sysfs dir moved OUT of the rootfs (files/pad-sysfs) — inside it proot's /proc/self/fd readback gave /sys/.pad/... and libudev dropped the device.
- Next (docs/steam-deck-controller.md "Next"): games get Steam Input's virtual pad (x86-64 libfakeinput in the FEX rootfs + evdev sysfs, test with Bastion); then touch controls in the compositor; then Steam's touch controller (0000:11fb).
- 12:02 Steam opens in Big Picture by default (-gamepadui); --desktop / menu action 'Restart Steam in Desktop Mode'. Commit 2f8b21f. Pushed to phone directly (no APK needed; next APK carries it).
- **12:40 touch-in-games investigation (Bastion):** native_touch: touches reach Xwayland as XI2 TouchBegin/End; Bastion (FNA/SDL2, which selects XI2 touch) ignores them, cursor doesn't move. native_touch=false (desktop mouse emulation, event_centralizer.rs/event_handler.rs): tap = instant press+release at lift point (FNA polls per frame → missed under FEX's low fps), hold = right click, drag = scroll — wrong for games. Keyboard works (held Enter reached the menu; instant adb keyevents can be missed for the same polling reason). Config restored to native_touch = true (effective at next app start).
- Design conclusion: a runtime touch mode in the compositor with a game mouse mode (finger down = move+press, move = drag, up = release; like gamescope touch-click-mode left), next to the on-screen-gamepad toggle. Also: the app rewrites guest/managed from the installed APK at each start — the Big Picture launcher change is NOT in the installed APK yet (re-copied by hand at 12:27).

## 2026-10-28 ~12:50: gamescope Game Mode — IN PROGRESS (user chose Game Mode session, like Steam Deck/DroidDeck)
- Cursor investigation closed: X pointer exact at every tap (±1 px); Bastion (old FNA) letterboxes 16:9 inside the 2176x1811 window and forgets the top bar → its cursor ~300 px low. User: treat as a screen-size problem for all games → gamescope.
- Installed in guest (hand-fetched, sha256-checked vs current ALARM DBs; guest DB is 10-01 so pacman -S 404s; no -Syu): aom 3.15.2, libyuv r2921, rav1e 0.8.1, libavif 1.4.2, luajit 2.1.1792588164, gamescope 3.17.0 (for deps/scripts/layer manifest). DroidDeck's patched gamescope (gamescope-3.16.29-p17, sha256 18e2b654…) at /usr/local/bin/gamescope + its patched WSI layer over /usr/lib/libVkLayer_FROG_gamescope_wsi_aarch64.so (stock 3.17.0 copy saved at /root/libVkLayer_FROG_gamescope_wsi_aarch64.so.stock-3.17.0). All of this is ad hoc on the phone, not in the repo yet.
- Nested in KWin works with DroidDeck's args: `gamescope --backend wayland --expose-wayland -f -W 2176 -H 1811 -w 1280 -h 720 -- …`. es2gears renders but flashes (GL → gamescope Xwayland, glamor sw). gamescope: "Failed to get DRM FD from renderer" (Turnip/KGSL has no VK_EXT_physical_device_drm) → no dmabuf in its wlserver.
- vulkaninfo/vkcube (default) fail everywhere: Turnip KGSL refuses VK_KHR_display ("I can't KHR_display", -3). Use `vkcube --wsi xcb`.
- In gamescope, vkcube --wsi xcb: WSI layer gets VkFormat_UNKNOWN (no formats) unless MESA_VK_WSI_DEBUG=sw; with sw it renders, letterboxed correctly, but SHAKES violently (old/new frames alternate). Removing kgsl-syncobj.so from gamescope's LD_PRELOAD: still shakes → not our syncobj emulation.
- Next: baseline vkcube --wsi xcb with MESA_VK_WSI_DEBUG=sw on the plain desktop (:0) to see if the shaking is Mesa sw WSI vs gamescope; then gamescope buffer release / wl_shm path.
- **~13:45 ROOT CAUSE of gamescope shaking FOUND + verified:** Turnip/KGSL has no VK_EXT_physical_device_drm → gamescope "Failed to get DRM FD" → no dma-buf for clients → sw copies (MESA_VK_WSI_DEBUG=sw / Xwayland without glamor) → out-of-order frames. Desktop sw path is smooth; shaking was gamescope-inbound only (SDL backend shook too; not kgsl-syncobj; 8 images = better, not fixed).
- **Turnip patch (in guest /root/src/mesa-26.2.3, NOT installed; originals in /root/src/*.orig, installed lib backed up /root/src/libvulkan_freedreno.so.before-drmnode):** tu_knl_kgsl.cc `kgsl_find_render_node()` — a /dev/dri/renderD* with the same st_rdev as the opened /dev/kgsl-3d0 (our app binds kgsl as renderD0) → has_local/local_major/minor; tu_device.cc `.EXT_physical_device_drm = !is_kgsl || has_local`; +#include <sys/sysmacros.h>. Plus an EXPERIMENT-ONLY `TU_KGSL_TEST_PRIMARY` env switch (reports the node as primary too) — must not be kept.
- gamescope 3.16.29 rendervulkan.cpp:481 requires a primary node unless the backend uses a Vulkan swapchain (nested Wayland doesn't); the primary dev id is otherwise optional (vulkan_primary_dev_id returns false). Proper fix = gamescope patch: require primary only for the DRM backend → needs our own gamescope build.
- Test: test ICD /tmp/tu-test.json → build-kgsl lib. With TU_KGSL_TEST_PRIMARY=1: no DRM FD error, no glamor fallback; `vkcube --wsi xcb` in gamescope WITHOUT MESA_VK_WSI_DEBUG=sw → user: "That's a smooth box!".
- Tools: ~/bin/ld-ordertest (synctest ramp mode, guest/tools/synctest.c 4th arg; desktop baseline 0 backward / 875 forward). synctest hangs inside gamescope on the sw path — retest with the dmabuf path.
- **~14:05 SMOOTH with the real pieces (user: "Smooth! very smooth"):** our gamescope 3.16.29-1 (guest/pkgbuilds/gamescope: Arch packaging + DroidDeck's 19 patches + ours 0200 `!UsesVulkanSwapchain() && IsSessionBased() && !hasPrimary`; built on the phone with guest/pkgbuilds/build.sh in ~3 min, installed with pacman; DroidDeck binary removed from /usr/local/bin) + our Turnip (patches/mesa/0001-turnip-kgsl-report-a-bound-render-node.patch, applies to pristine mesa-26.2.3; built in guest build-kgsl, copied over /opt/turnip-kgsl/lib/libvulkan_freedreno.so; old lib at /root/src/libvulkan_freedreno.so.before-drmnode). No MESA_VK_WSI_DEBUG, no test env.
- First real run CRASHED KWin (segfault, signal 11 at 0x1, 13:55:45) → whole session died; KWin was still running the OLD Turnip it loaded at session start while gamescope used the new one. After an app restart (everything on the new Turnip) no crash in 2 runs. Not root-caused; watch for it.
- build.sh caveat: ld-run prints "[exit 1]" but build.sh exits 0 on a failed build.
- Build tools hand-installed in guest (sha256-checked vs current ALARM DB): cmake git vulkan-headers catch2 zlib-ng perl-mailtools perl-timedate perl-error rhash libuv jsoncpp cppdap.
- Reminder: app restart rewrites guest/managed from the installed APK (ld-20261028-c-pad), which predates 2f8b21f (Big Picture) → re-copied localdesktop-steam by hand again at ~14:00. Next APK fixes it.
- Not yet: commit (gamescope pkgbuild, mesa patch, synctest ramp), repack guest/blobs/turnip-kgsl.tar.xz from /opt/turnip-kgsl, ship gamescope in the APK/guest setup, ld-ordertest number in gamescope, Game Mode session.
- **~14:15 PACKAGED + COMMITTED:** bf108f9 (gamescope build + guest/blobs/packages + localdesktop-packages at session start, IgnorePkg gamescope), c03f947 (synctest ramp + ld-ordertest; msg says "fifth argument", it is the 4th), e40dae3 (gamepad requirements doc), d327583 (Turnip render-node patch + guest/build-turnip.sh + rebuilt blob 7fe72f3f…; full phone build from the sha256-checked release worked). APK **ld-20261028-d-gamescope** installed + archived: clean start verified (build id, Turnip lib hash = blob, -gamepadui launcher, 6 packages, gamescope 3.16.29-1 ours, nothing reinstalled), vkcube in gamescope swapchain created, KWin alive.
- Next: Game Mode session (Steam Big Picture inside gamescope, -steamdeck -steamos3, --default-touch-mode, launcher from Plasma, exit back) → then Bastion touch inside gamescope (scaling + touch mapping), ld-ordertest number in gamescope.
- **~14:30 Game Mode WORKS (b988e9e, 69ea179):** menu entry "Steam Game Mode" (localdesktop-gamemode): gamescope nested fullscreen, Steam -gamepadui -steamdeck -steamos3, --default-touch-mode 4, games at the display's native size (2176x1811). User: touch works smoothly; Bastion cursor under the finger. Steam's per-game "Game Resolution" is available in Properties → user leaves Bastion native. Switch to Desktop untested (no Steam pill/menu reachable by touch in the user's test). Preference saved: native resolution, scale per game.
- Pushed to phone by hand, in commits, NOT in the installed APK (ld-20261028-d-gamescope) yet: localdesktop-gamemode, steamos-session-select, localdesktop-steam --gamemode/--shutdown, the .desktop entry. Next APK carries them.
- Next on the roadmap: games see Steam Input's virtual pad (Bastion), then the on-screen gamepad with toggle.
- **~15:15 on-screen gamepad DISABLED (user request) — 02363f5, build ld-20261028-f-pad installed:** gamepad.rs exists but off unless /root/.config/localdesktop/gamepad-enabled. Problems seen: drawn once (14:37), later not drawn while its controls still took touches (dead zones); STEAM button dims the game but no menu appears (gamescope overlay layer?); touching pad controls made the screen flash. Logs "gamepad: Game Mode started/ended (flag, pid)" in logcat for the investigation. Arrows now drawn as triangles, bottom row at y=0.86.
- Steam menu entry = Game Mode (ff0df1b); pid-file detection (3adefc2, gamescope's comm is gamescope-wl); reopening Steam raises the minimized gamescope window via a KWin script (qdbus6 loadScript).
- Bastion: in Game Mode at native res its own letterbox bug offsets the cursor again → set Game Resolution 1280x720 per game. Separate: one launch crashed in GSGE.WeaponData.readWeaponXml (NullReference) on Play; next launch worked.
- User priority now: stable Game Mode with touch first; gamepad later.
- 15:20 launch 'failure' = ~30 s start: channel fight (Steam's package/beta = steamdeck_stable vs forced publicbeta → reinstall each start, 16 s) + 11 s normal start; user's 2nd tap at +10 s hit 'already running'. Fixed (follow package/beta, commit ae26333); raise on reopen (02363f5). Bars at top/bottom were Plasma panels during the launch transition only.
- **~16:00 Bastion clicks:** in the 15:38 Game Mode instance taps moved Bastion's cursor but never clicked (xinput test on xwayland-pointer: 220 motion, 0 buttons, even with touch_click_mode forced to 1 by xprop and gamescopectl). Fresh instance started with WAYLAND_DEBUG=1: per tap KWin sends wl_touch.down and gamescope sends wl_pointer.button(272, pressed) to Xwayland → clicks work (user confirmed). Cause of the bad instance unproven; guess: stale pointer focus after Bastion changed resolution/recreated its window. If it recurs: capture with WAYLAND_DEBUG and /tmp/trace.sh <line>.
- Game Mode at 16:9 (2176x1224, 427049b) + Bastion 1920x1080 → touch position correct (user). Bastion at non-16:9 resolutions letterboxes itself → offset; options offered (Steam Game Resolution match, windowed mode, FNA update).
- Launch reliability: 8d24aa2 (KWin script raises gamescope's window when it appears + "Steam is starting" notification); ae26333 channel fix. All on the phone; next APK needed for guest/managed changes since ld-20261028-f-pad.

## 2026-10-28 ~16:05 — CONSOLIDATED STATE (read this first)
Branch perf/gpu-compositing-pacing, tree clean, not pushed. Installed APK **ld-20261028-f-pad** (archived in ~/localdesktop-bench/apks/). guest/managed changes made after it (localdesktop-gamemode, localdesktop-steam-install, the Steam .desktop) were pushed to the phone by hand and are in commits → **the next APK must be built before they survive an app restart** (the app rewrites guest/managed from the installed APK on every start).

### Today's commits (oldest first)
7176070 proot 0019 execveat · ba85d4d Steam arm64 client + lsof/loopback-owner + gtk2 · 64fa747 FEX (localdesktop-fex, rootfs copied, PATH) · 49eaf89 emulated Steam Deck controller (libfakeinput, localdesktop-pad, files/pad-sysfs binds) · 2f8b21f Big Picture default · bf108f9 our gamescope build shipped (guest/pkgbuilds/gamescope, guest/blobs/packages, localdesktop-packages) · c03f947 synctest ramp + ld-ordertest · e40dae3 gamepad requirements doc · d327583 Turnip render-node patch (patches/mesa/0001, guest/build-turnip.sh, new blob) · b988e9e Game Mode · 69ea179 native res (superseded) · ff0df1b Steam entry = Game Mode · 3adefc2 pid-file detection · 02363f5 on-screen gamepad (OFF) + raise on reopen · ae26333 follow Steam's channel · 427049b games at largest 16:9 fit · 8d24aa2 auto-raise window + "Steam is starting" notification.

### Works (verified on the phone)
- Steam (arm64 client, channel now steamdeck_stable via Steam's package/beta) from the app-menu "Steam" entry → Game Mode: gamescope nested in KWin, Steam -gamepadui -steamdeck -steamos3, fullscreen; window auto-raised when it appears (gamescope opens it only once Steam draws, 10–60 s).
- gamescope smooth with dma-buf (root cause of shaking: Turnip/KGSL lacked VK_EXT_physical_device_drm → sw copies out of order). Our gamescope 3.16.29 + DroidDeck's 19 patches + 0200 (no primary node needed when nested).
- Games screen = largest 16:9 fitting the display at native density (2176x1224 on the Fold inner). Touch mapping inside gamescope measured exact (±1 px at 3 points). Bastion at 1920x1080: cursor under the finger; clicks reach it (WAYLAND_DEBUG trace: wl_touch.down → wl_pointer.button 272 pressed).
- Bastion (x86 Linux, FNA) via "Linux x86 (FEX)"; Deck controller drives Steam's UI (d-pad, Steam button) via localdesktop-pad.

### Known problems / open
1. **Clicks stopped in one Game Mode instance** (cursor moved, 0 button events at Xwayland even with touch_click_mode forced). Fresh instance fine. Unproven guess: stale pointer focus after Bastion changed resolution. Recipe if it recurs: start Game Mode with WAYLAND_DEBUG=1, mark `wc -l /tmp/gamemode.log`, tap, `/tmp/trace.sh <line>` (greps wl_touch/button/enter/leave).
2. **Self-letterboxing games** (Bastion at non-16:9 res, e.g. 1600x1200, or at the display's own shape) misplace touch by their own bars — not fixable below the game. Options given to the user: Steam per-game Game Resolution matching the in-game res; game windowed mode inside gamescope (untested); newer FNA for Bastion (unverified that it fixes it). Bastion fullscreen at non-native sizes also looked squashed/blurry.
3. **On-screen gamepad** (src/android/backend/wayland/gamepad.rs) OFF unless /root/.config/localdesktop/gamepad-enabled: drawn at first, later not drawn while still taking touches (dead zones); its STEAM button dimmed the game but Steam's menu/QAM never appeared (overlay windows not shown in nested gamescope?); touching it made the screen flash. Logs "gamepad: Game Mode started/ended" in logcat. Arrows drawn as triangles, bottom row at y=0.86 (untested).
4. "Switch to Desktop" (steamos-session-select) untested; Steam's menu not reachable by touch (needs the pad or a controller).
5. Plasma Mobile's launcher may not show .desktop Actions (long-press "Restart Steam"/"Steam in Desktop Mode" not confirmed). To restart Game Mode: close it in the task switcher, tap Steam.
6. Bastion crashed once on Play in GSGE.WeaponData.readWeaponXml (NullReference); next launch fine.
7. KWin crashed once (13:55) when gamescope used the new Turnip while KWin still had the old one loaded; not seen since the APK shipped the new Turnip.
8. Games don't see Steam Input's virtual pad yet (x86-64 libfakeinput in the FEX rootfs + evdev sysfs not wired).

### Useful commands / tools
- Launch from the session environment: `ld-run 'P=$(pgrep -x plasmashell|head -1); (cd /; xargs -0 -a /proc/$P/environ sh -c "exec env -i \"\$@\" setsid nohup localdesktop-gamemode </dev/null >/dev/null 2>&1" sh) &'`.
- gamescope pid: /run/localdesktop-pad/gamemode (process comm is gamescope-wl, not gamescope). gamescope X display :1 (query with ld-run, not ld-exec: auth). `DISPLAY=:1 xprop -root | grep -E "TOUCH|FOCUSED"`.
- KWin scripting over D-Bus (qdbus6 org.kde.KWin /Scripting loadScript/run/unloadScript) to inspect/raise windows; print() lands in /tmp/plasma-mobile-session.log as "js: …".
- Logs: /tmp/gamemode.log (gamescope), /tmp/steam-run.log, ~/.local/share/Steam/logs/{console_log,controller,compat_log,webhelper}.txt, /run/localdesktop-pad/pad.log.
- Builds: `make build-docker BUILD_ID=…` (~70–250 s), guest/pkgbuilds/build.sh <name> (gamescope ~3 min on the phone; exits 0 even when the build fails — check its output), guest/build-turnip.sh (full Mesa on the phone), guest/build-fakeinput.sh (docker, bullseye).
- Hand-installed in the guest (not in the APK): build tools cmake git vulkan-headers catch2 (+deps), xorg-xinput xdotool. Guest pacman DB is from 10-01; `pacman -S` 404s — fetch packages by hand, sha256 against the current ALARM DB.

### Next (user decides)
Rebuild APK; investigate/guard the stuck-clicks instance; Bastion non-16:9 options; on-screen gamepad (render/overlay/flash bugs) with the toggle; games see the virtual pad; then Proton ARM64 / Windows games.

## 2026-10-29 ~08:35 — CONSOLIDATED STATE (read this first)
Branch perf/gpu-compositing-pacing, tree clean at **45a2dff**, not pushed. Installed APK **ld-20261029-d-steamjob** (archived in ~/localdesktop-bench/apks/), built from 45a2dff: the phone's guest/managed files match the commit (checked by hash). Everything from 10-28 (below) still holds unless noted.

### Fixed today: "Steam from the app drawer doesn't appear"
- Cause 1: the app restarted (07:40) and rewrote guest/managed from the older APK (f-pad), dropping the post-APK commits (auto-raise KWin hook, notification, channel fix). Lesson: **build + install an APK after any guest/managed change before the app restarts**; hand-pushed files don't survive. Check: hash-compare guest/managed against the phone (ld-exec loop over the files).
- Cause 2: the old localdesktop-steam-install forced -clientbeta steamdeck_publicbeta while Steam's package/beta said steamdeck_stable → Steam reinstalled its whole client (430–506 MB, ~4 min under FEX) on every start, invisibly. Yesterday's 17:22/19:30/20:11 starts were all killed mid-update. With ae26333 installed: one stable update (08:02:24–08:05:39), then the next start had no download, client up ~9 s after the tap.
- Cause 3: nothing visible during the update. Now **localdesktop-steam-progress** (python3 + gi) shows a Plasma job via org.kde.JobViewServerV2.requestView (desktopEntry localdesktop-steam, hint transient=true): step text (setInfoMessage), bar (setPercent: download % ×0.6, extracting 65, installing 80, "Opening Game Mode" 90/95), download bytes under Details. The user confirmed it visually.
  - Plasma ignores the notify-send `value` hint (not in GetCapabilities): no bar. Jobs die with the requesting D-Bus connection, so one-shot gdbus/qdbus calls can't hold a job; a long-lived process is needed.
  - End marker: `setting LIBGL_KOPPER_DISABLE=true for steamwebhelper` (the real client) then the next `Gamescope WSI] Created swapchain`. The first swapchain ~2 s after the tap is the updater's -child-update-ui window: don't end on it.
  - Steam log lines can end in \r. "Set percent complete: -1" = no percentage (extract/install give none).
  - Second tap while starting: localdesktop-gamemode sends SIGUSR1 to the pid in /run/localdesktop-pad/gamemode-starting → new popup. SIGTERM/SIGHUP end the job (tested: no leftover JobView).
  - python-gobject added to the localdesktop-install-plasma-mobile PACKAGES (on this phone it came only via onboard); without it a plain notify-send.
- Screenshots: `adb exec-out screencap -d 4630947175568309891 -p` (inner display; plain screencap warns about multiple displays and corrupts the PNG).
- ld-exec gotcha: `pkill -f <pattern>` inside ld-exec matches ld-exec's own sh -c and kills it.

### Open (unchanged from 10-28 list): stuck-clicks instance, self-letterboxing games, on-screen gamepad bugs, games don't see the virtual pad, Proton/Windows games. Not yet seen live: a real Steam client update through the new progress job (only replays of the 07:41 log).

### Next (user decides)
**CONTROL + REPOS (2026-10-29 ~09:20; the user's decisions):**
- Control baseline: tag `control-2026-10-29` = the commit "docs: control build ld-20261029-e-agnostic" on perf/gpu-compositing-pacing, with APK **ld-20261029-e-agnostic** (~/localdesktop-bench/apks/ and ~/localdesktop-bench/control/). Every further fix or feature starts on a new branch from this tag and is compared against it. Restore: `adb install -r ~/localdesktop-bench/control/ld-20261029-e-agnostic.apk`.
- Remotes: `fork` = the user's **public** fork github.com/MCJennings93/localdesktop.github.io (primary; the branch tracks it; the fork's default branch is still `main`). (The private mirror `mine`, localdesktop-private, was retired on 2026-10-30.) `origin` = upstream, read-only. Push only when asked.
- Published history was rewritten twice (from e6a8b58, 38 commits): (1) author/committer email → 203696549+MCJennings93@users.noreply.github.com (repo-local user.email set to it); (2) **device-agnostic wording**: the user said the project isn't specific to the test phone, it's for any Android device. So no model names in published files, notes or commit messages ("the test phone", "a near-square foldable screen"); the notes copy is docs/dev-notes.md, with the adb serial replaced by <adb-serial>. Old histories are kept locally only in branches backup/pre-email-rewrite and backup/pre-agnostic-rewrite (plus backup/* tags). Never push those.

## 2026-10-29 ~09:45 — upstream merge test (branch test/upstream-except-328, local only)
- Branch from control-2026-10-29: merged upstream up to 6df9af2 (7 commits), cherry-picked the 9 after #328 (#328 = fafbdad, upstream's own Adreno GPU work, left out on purpose). Conflicts: .gitignore (both kept), Cargo version lines (took upstream: 3.0.5), libproot.so (kept ours: already uses process_vm_readv; took config API=23), accessibility.rs (both functions kept).
- Package is now io.github.localdesktop; test build labelled "Local Desktop 3", installed NEXT TO app.polarbear (separate data, fresh setup). Device tools take LD_PKG (default app.polarbear). ~/bin/ld-ordertest and ld-racetest were stale copies; now symlinks like the rest.
- Found + fixed (commit bc1ef79, test branch only): the setup page's WebView popup was built on its own thread, so any configuration change during first-time setup (rotation, fold, font scale) crashed with CalledFromWrongThreadException. Pre-existing upstream bug (control and upstream have it; not the target-SDK change: SDK 33 probe crashed too). SetupPage.java builds it on the main thread, one popup per activity. Repro/test: `adb shell settings put system font_scale 1.05` during setup, then restore 1.0.
- Installed test APK: ld-20261029-t5-setuppage. Java → classes.dex is built in the alvrme/alpine-android:android-30-jdk17 image, piping sources via tar on stdin (SELinux blocks bind mounts).
- Candidate upstream PR from the fork: the SetupPage fix.
- ~10:30 B (io.github.localdesktop, "Local Desktop 3", APK ld-20261029-t7-pmfix) is UP on Plasma Mobile: fresh guest = current Arch (KWin/Plasma 6.8.2, Qt 6.12.0, Mesa 26.2.6; 285 packages newer than A). KWin composites on OpenGL ES via Zink on Turnip (Adreno 740); Brave (kgsl-syncobj explicit sync active) and Firefox smoke tests clean (35 s each, no crash markers). Mesa 26.2.6 has NOT shown DroidDeck's "Zink crashes with Turnip after 26.2.4" so far. Added to B: /etc/localdesktop/test-runner, timezone America/Chicago. B was brought to Plasma by pointing localdesktop.toml's launch at the installer (runs with network in the app domain; output in /tmp/localdesktop-session.log).
- Fresh-install bugs found (both in our fork, not the merge): (1) Xfce default session dies: labwc on our fake KGSL render node → Mesa can't name the driver → software EGL refused, no Vulkan ICD → "unable to create renderer" (control has it too; NOT fixed; fix = share the GPU env from startplasmamobile with the Xfce session). (2) Plasma installer lacked pipewire-pulse (IgnorePkg pin) → fixed edc2cd3. Also dbe17e7: session output → /tmp/localdesktop-session.log.
- User's plan (10:25): first get B stable on its new software, THEN update control A's packages (pacman -Syu) to match. A/B goal = review the merged commits against control. No Steam/FEX in B for now.
- ~10:40 B app drawer was EMPTY: Plasma Mobile 6.8 Folio builds the list from KServiceGroup::root() (the XDG menu); our session never set XDG_MENU_PREFIX and KService 6.31 dropped the fallback to plasma-applications.menu (6.30 in A still has it). Fixed in startplasmamobile-localdesktop (commit on test branch): export XDG_MENU_PREFIX=plasma- + one-time `kbuildsycoca6 --noincremental` keyed by ~/.cache/localdesktop-menu-prefix. A will need this when its packages are updated. B APK: ld-20261029-t8-menuprefix.

## 2026-10-29 ~10:55 — A/B setup (user: "have both copies stable and ready for comparison")
- A = app.polarbear "Local Desktop", branch **control-updated** = control-2026-10-29 + our own fixes (4548c0c LD_PKG tools, 910d998 session log, fc4ac5d installer pipewire-pulse, 1e5c858 XDG_MENU_PREFIX). APK ld-20261029-a1-control-updated. control-2026-10-29 tag/branch untouched.
- B = io.github.localdesktop "Local Desktop 3", branch test/upstream-except-328 = control + 16 upstream commits (no #328) + the same four fixes + B-only: SetupPage fix (bc1ef79, tied to upstream's io.github.localdesktop dex) and the "Local Desktop 3" label. So A vs B differ by the upstream commits (+ setup page, only used during first-time setup).
- A's guest updated to current Arch (pacman -Syu, 303 packages, PipeWire family explicitly to 1:1.6.10 to match B; gamescope stays pinned at our 3.16.29). Rollback snapshot first: ~/localdesktop-bench/snapshots/A-before-update-20261029/ (pacman -Q list, tar of etc + var/lib/pacman/local + root/.config + root/.local/share minus Steam) + A's pacman cache keeps the old packages.
- ~12:40 A UPDATED + VERIFIED: pacman -Syu finished 11:03 (303 pkgs, exit 0; phone disconnected mid-way but it completed). A: KWin/Plasma 6.8.2, Qt 6.12.0, Mesa 26.2.6, KService 6.31, PipeWire 1.6.10; KWin OpenGL ES on Zink/Turnip; drawer OK (menu fix); Brave (explicit sync) + Firefox smoke clean; no crashes. Package compare A vs B: 0 version differences; only in B: squeekboard + 3 deps (upstream #329, Xfce only); only in A: 58 (Steam deps, build tools, onboard, xdotool…). BOTH READY FOR A/B.
- ~12:50 MERGED-COMMIT TESTS (goal per user: test the added function and stability of the merged commits):
  - #334 rename: B coexists with A ✓; typing works (user) → rebuilt libxkbcommon OK ✓.
  - #335/#338 target SDK 36: both screens incl. fold/cover work (user) ✓; rotation 0→90→0 on A and B via user_rotation: both survive, guest output follows 1812x2176 ↔ 2176x1812 scale 3 identically ✓; screenshots: identical fullscreen layout, no inset/edge-to-edge offset in B ✓. (Restore: accelerometer_rotation=1 user_rotation=0.)
  - #339: KeyboardAccessibilityService still declared + registered in both debug builds ✓. #340 prompt + shortcut forwarding: UNTESTED (no physical keyboard; a gamepad doesn't count as a keyboard).
  - No device effect: #312 #317 #336 #337, #331 (kept our proot), #329 (Xfce only; Xfce broken by our render-node bug). VS Code extension commits (#321 #324 #325 #327 #330) are PC-side, not tested.
  - Crash buffer empty across all tests. Remaining: user's hands-on soak of B vs A.

## 2026-10-29 ~14:00 — ADOPTED the merge; new control; app layout on the phone
- Main branch perf/gpu-compositing-pacing = control-2026-10-29 + 16 upstream commits (no #328) + our fixes + label reverted; tag **control-2026-10-29-merged**, APK **ld-20261029-m1-merged** (package io.github.localdesktop).
- Phone layout (user's decision): **"Local Desktop" = io.github.localdesktop = THE CONTROL** (A's Linux system copied in, Steam signed in, Bastion works). **"Local Desktop B" = app.polarbear** (reused; still holds the original Linux system) = graphics-testing copy for upstream #328.
- Migration method (works, 2.5 min for 26.8 GB): ON the phone, detached (`setsid nohup sh /data/local/tmp/migrate.sh &`), `run-as A tar -c | run-as B tar -x` with the guest's GNU tar run through files/ld-linux-aarch64.so.1 --library-path …/usr/lib (toybox tar can't extract into read-only dirs). Streaming through the PC was slow (11 MB/s) and died when USB dropped. Two unreadable items (dbus-daemon-launch-helper ---s--x---, /run/systemd/dissect-root d---------) filled from the fresh guest. Verified 525,195 entries both sides. B's fresh guest kept as files/arch.fresh-20261029 in io.github.localdesktop.
- The merged build's setup check includes squeekboard (#329) → first start ran the install step on the copied system (squeekboard + 3 deps, device-mapper), then "restart the app".
- Device tools now default LD_PKG=io.github.localdesktop.
- TABLED (user, 14:00): Switch Pro Controller in Steam. Findings: Android pairs it (hid-nintendo, 057e:2009, sources KEYBOARD|GAMEPAD|JOYSTICK, positional buttons = Deck layout, axes X/Y Z/RZ, ZL/ZR digital, D-pad keys + HAT, IMU sensors via InputDevice sensor API). Today D-pad works only because Android sends arrow keys. Plan: winit patch emits gamepad events instead of keys/mouse → compositor merges into the pad ring (localdesktop-pad/libfakeinput Deck controller); Capture = Steam button (Android keeps Home), Capture+− = QAM; then games-see-virtual-pad (x86-64 libfakeinput in FEX rootfs).
- NEXT (user's first priority): A/B the two graphics stacks: ours (this week, from DroidDeck) vs upstream main's #328, in "Local Desktop B".

## 2026-10-29 ~15:05 — B ("Local Desktop B", app.polarbear) runs upstream's GPU stack (#328)
- Branch test/graphics-328-b = control-2026-10-29-merged + package app.polarbear/label + merge of fafbdad (#328) + fix/arch-fs-root. APK ld-20261029-b3-upstream-gpu.
- B = upstream's frame loop (watcher thread, AChoreographer FrameClock, frame callbacks on vsync, idle scheduling) + hardware_buffer.rs (shm rows → AHardwareBuffer) + their Mesa in /opt/localdesktop-mesa via env.sh (vkprobe gate; VK_ADD_DRIVER_FILES, glvnd vendor, zink override, MESA_VK_WSI_DEBUG=sw, LIBGL_KOPPER_DRI2=1). Ours switched off in B: no linux-dmabuf global, no fake render node (process.rs RENDER_NODE=false), no GPU shims/our Turnip in the session, KWin on QPainter. Kept (not compared): our proot, Plasma Mobile, native touch, scale handling, gamepad overlay, presentation feedback, frame_stats, BRAVE_FLAGS. Verified on device: KWin 6.8.2 KWIN_COMPOSE=Q, eglinfo in session = zink on Turnip (upstream Mesa).
- Without our render node the guest sees Android's real /dev/dri (card0, renderD128), which the app can't open.
- BUG FOUND + FIXED (branch fix/arch-fs-root, cd405f4 + 2bf91ec; merged into B; NOT yet on main): config.rs hard-coded ARCH_FS_ROOT=/data/data/io.github.localdesktop/files/arch (upstream #334) → any other package name looked in the main app's dir ("Failed to rename extracted files to final destination"). Now set at startup from getFilesDir (canonicalized to /data/data/<pkg>/files/arch). Also libxkbcommon is built with that package's XKB/locale roots → B segfaulted in xkb_keymap_new; the app now sets XKB_CONFIG_ROOT and XLOCALEDIR from its own root.
- CONTROL REPAIRED: proot's link2symlink stores ABSOLUTE host paths; 4,013 links in the migrated control still pointed at /data/data/app.polarbear/files/arch (terminfo, zoneinfo → clock showed UTC, unzip broken, qt6, git, perl…). Repointed all to /data/data/io.github.localdesktop/files/arch with an on-device script (GNU find -lname via ld-linux, ln -sfn, temporary u+w on read-only parents); verified 0 left, 0 broken, CDT, unzip OK. ANY future copy of a rootfs between packages needs this relink step.
- ~15:20 fix/arch-fs-root MERGED into main (65a3672) and installed on the control as ld-20261029-m2-relative-paths: Plasma up 9 s, no crash, CDT, KWin still OpenGL ES on Zink/Turnip. New control tag **control-2026-10-29-v2**. The rootfs path and libxkbcommon's XKB/locale roots now follow whatever package name manifest.yaml gives.
- ~15:45 GRAPHICS A/B RESULT #1: upstream's #328 stack CANNOT run Plasma Mobile. In B, KWin 6.8 logs "Configured compositor not supported by Platform. Falling back to defaults / The used windowing system requires compositing / We are going to quit KWin now as it is broken": KWin 6.8's nested Wayland backend has no QPainter, and OpenGL needs a render node/dmabuf from the host, which #328 deliberately lacks. Screen stays dark red; the A/B framebench in B recorded 0 frames. (Found with temporary loop counters: watcher sent 1 wake, dispatch ran, toplevel created, KWin never attached a buffer; WAYLAND_DEBUG on KWin; both removed again.) #328 targets upstream's Xfce/labwc (pixman) desktop. A's numbers (Plasma, our stack): cold start 7.3/7.0/5.7 s; drawer anim ~22 fps SF avg, render ~1 ms; es2gears ~82 fps, app 271 cs/10 s; Brave+shade: 12.2% janky, app CPU 128% of a core. B cold starts 7.7/7.1/5.4 s (to plasmashell, which runs without KWin's output).
- ~16:15 EXPERIMENT 3a (branch exp/upstream-frame-loop 8620208: upstream's watcher + AChoreographer vsync frame loop on OUR stack), measured in the B slot against the control code in the same slot (B0, slot commit 06c59af), 2 runs each: es2gears SF 69/73 → 101/98 fps (app-side 58-66 → 109-115), frames ≥12 ms apart 429/729,397/760 → 259/1084,288/1026; drawer ~19-20 fps both; Brave+shade jank 12.3/12.9% → 14.0/12.5% (noise); Brave CPU 128/129% → 134/151% (to confirm); cold start same; 0 crashes. → 3a is a clear win for continuously animating GL clients. NB: same control code in A's slot gave es2gears 82 fps vs 69-73 in B's slot → always compare within one slot. ld-framebench's KWin CPU is unreliable (0 or 426 cs).
- 3b (bf…: hardware_buffer.rs on top of 3a) built, committed; only affects wl_shm clients → measure under Xfce (step 1).
- NEXT (user: "Do 3 then 1"): step 1 = fix Xfce on our stack (labwc on the fake render node), then Xfce A/B: ours (± 3a/3b) vs upstream #328 in B.
- ~16:50 STEP 1 (Xfce on our stack), branch fix/xfce-gpu: (1) 6cecc39 shared /usr/local/lib/localdesktop-gpu-env.sh sourced by both sessions (labwc got "failed to get driver name" → software EGL refused); (2) a9e9ad5 kgsl-drm-shim answers DRM_IOCTL_AUTH_MAGIC with EACCES like a render node (libdrm drmIsMaster → wlroots tried drmModeCreateLease); (3) …: DRM_CAP_PRIME → IMPORT|EXPORT (wlroots GBM allocator "PRIME export not supported"). Now labwc starts and stays up, creates its xdg_toplevel, attaches a 24x24 dmabuf cursor buffer, then never commits; main thread in do_sys_poll → likely waiting on a GPU fence (implicit/explicit sync over KGSL). OPEN. Trace method: temporary localdesktop.toml launch with WAYLAND_DEBUG=client (restored). Plasma session in B (control code + the gpu-env refactor) not yet re-verified.
- ~17:15 XFCE A/B (labwc on pixman; B slot; tools: run-xfce.sh, SHELL_PROC=xfce4-panel, runner /root/ld-runner.sh started from the session's localdesktop.toml; configs /root/toml-xfce-ours|up, /root/toml-plasma):
  es2gears SF fps / Brave+shade jank / Brave CPU: X-ours (control loop) 37.9 / 12.7% / 163%; X-3a (upstream loop) 25.7 / 13.4% / 166%; X-3b (+hwbuf) 25.0 / 13.4% / 168%; X-up (upstream #328 as shipped) 24.5 / 12.9% / 170%. 1 run each, 0 crashes.
  → Upstream's frame loop costs ~1/3 with a CPU-compositing nested compositor (labwc pixman) but gains ~40% with KWin on the GPU (Plasma 70→100). Callbacks only at vsync + one render per refresh serialize labwc's slow CPU frames; ours sends callbacks as soon as commits are taken (pipelines). 3b (AHardwareBuffer upload) no measurable gain here. With the same loop, our stack ≈ upstream's (25.0 vs 24.5). Our stack additionally runs Plasma/KWin on the GPU, which #328 cannot.
  Xfce on our stack works only with WLR_RENDERER=pixman; labwc on the GPU still stalls in its first frame (fence). B currently on X-up config (upstream #328 build b3 + toml-xfce-up): a working "upstream stack" desktop.

## 2026-10-29 ~17:00 — CONSOLIDATED STATE (read this first)
User's plan: both graphics options stable now; review the A/B results and decide next session — likely MERGING THE TWO STACKS. No merges into main from the A/B work yet.

### Phone (both verified at ~16:56, 0 crashes)
- **"Local Desktop"** = io.github.localdesktop = CONTROL, tag **control-2026-10-29-v2** (main branch perf/gpu-compositing-pacing), APK ld-20261029-m2-relative-paths. Plasma Mobile, KWin OpenGL ES on Zink/our Turnip (render node + dmabuf zero-copy), Steam signed in, CDT.
- **"Local Desktop B"** = app.polarbear = UPSTREAM #328 STACK, branch test/graphics-328-b, APK ld-20261029-b3-upstream-gpu, running **Xfce** (Plasma impossible with #328: KWin 6.8 has no QPainter nested and no GPU device → quits). Session config /etc/localdesktop/localdesktop.toml = /root/toml-xfce-up (WLR_RENDERER=pixman + test runner /root/ld-runner.sh). Other configs there: /root/toml-xfce-ours, /root/toml-plasma. Apps render on Zink/upstream Turnip; Brave+Firefox OK.

### Results (details in the sections above; data in ~/localdesktop-bench/ab-20261029/)
| Setup | es2gears (SF fps) | notes |
|---|---|---|
| Plasma, control code (B slot, 2 runs) | 69 / 73 | (82 in A's slot — compare within one slot) |
| Plasma, ours + upstream frame loop 3a (2 runs) | 101 / 98 | Brave jank same, CPU maybe +5-20% |
| Plasma, upstream #328 | — | KWin quits: no desktop |
| Xfce/labwc pixman, ours + control loop | 37.9 | Brave jank 12.7%, CPU 163% |
| Xfce, ours + 3a | 25.7 | |
| Xfce, ours + 3a + 3b (hwbuf) | 25.0 | |
| Xfce, upstream #328 as shipped | 24.5 | Brave jank 12.9%, CPU 170% |
Conclusions: the stacks perform the same with the same frame loop; upstream's loop helps GPU-compositing KWin (+40%) but hurts CPU-compositing labwc (−1/3); 3b no measurable gain; ours is required for Plasma/KWin.

### Branches (all local; backup bundle ~/localdesktop-bench/control/all-branches-20261029.bundle)
- exp/upstream-frame-loop: 3a (8620208) + 3b (687230e) on control-v2.
- fix/xfce-gpu: shared /usr/local/lib/localdesktop-gpu-env.sh (6cecc39), kgsl-drm-shim AUTH_MAGIC→EACCES (a9e9ad5) and DRM_CAP_PRIME (19a30ff), tools SHELL_PROC (85a427b). Xfce works with labwc on pixman; labwc on the GPU stalls in its first frame (fence) — OPEN.
- test/graphics-328-b: B (upstream #328 merged, ours off). slot/* branches = experiment + test-slot commit (app.polarbear label "Local Desktop B").
### Open decisions (next session)
1. Merge the stacks: e.g. our stack + a hybrid frame loop (our early frame callbacks + upstream's vsync pacing/idle) measured on Plasma AND Xfce. 2. Merge fix/xfce-gpu (with labwc pixman default until the fence stall is fixed)? 3. Keep or drop 3b. Also tabled: Switch Pro Controller in Steam.

## 2026-10-29 ~18:00 — Hybrid C built, smoke-tested, full A/B/C running
- Branches (local): exp/frame-callback-trace (3c86924: frame-stats cb[redraw/vsync/commit, wait = commit→release, produce = release→next commit]; on fix/xfce-gpu) → exp/hybrid-c (496f074 3a + trace hook in on_vsync; 98a8303 HYBRID: in dispatch_clients, release callbacks at once when ≥1 refresh since the last release, else at vsync; WaylandBackend.callbacks_released_at). Slot builds (app.polarbear "Local Desktop B"): ld-20261029-abc-A-trace (control loop), abc-3a-trace (upstream loop), abc-C (hybrid); upstream B = ld-20261029-b3-upstream-gpu.
- Scripts: ~/localdesktop-bench/abc-next/run-abc.sh [rounds] (installs each build into B, sets /root/toml-*, runs run-plasma.sh/run-xfce.sh round-robin, then summarize.sh), restore-b.sh (back to upstream #328 + Xfce). Stay-awake: run sets `svc power stayon usb`, original saved in abc-next/stay_on.orig and restored at the end.
- C smoke test 17:5x: Plasma (KWin, plasmashell up; cb commit=29 vsync=8) and Xfce (labwc, xfce4-panel; commit=7-12 vsync=2-4), 0 crashes.
- Full run started ~18:00: 3 rounds × 7 configs (A/3a/C on Plasma and Xfce, B-up on Xfce). Results: abc-next/results-<date>/summary.txt.
- ~18:25 STOPPED for the session (user). The full run had a bug: `echo "$CONFIGS" | while read` loop — adb/scripts inside read stdin and ate the config list, so each round ran only A-plasma. FIXED in run-abc.sh (every command < /dev/null), not yet re-run. Only result: A-plasma r1 (abc-A-trace): SF 69.2 fps, ≥12 ms 433/723, app 58.4 fps, Brave+shade jank 17.8%, CPU 115%, cold 7.1/6.9/6.8 s, cb: 671 redraw releases, wait 0.2 ms, produce 15.7 ms (KWin takes ~16 ms after release → the ~60-70 fps ceiling of our loop on Plasma).
- Phone left clean: stay_on_while_plugged_in restored to 0; "Local Desktop B" restored to upstream #328 (b3) + Xfce (restore-b.sh); "Local Desktop" control untouched (v2).
- NEXT SESSION: `~/localdesktop-bench/abc-next/run-abc.sh 3` (~90 min, phone plugged in, untouched; it sets/restores stay-awake only if wrapped as on 10-29: run `adb shell svc power stayon usb` before and restore `settings put global stay_on_while_plugged_in <abc-next/stay_on.orig>` after), then decide on merging the stacks.
- ~18:27 OVERNIGHT REAL-WORLD TEST (user): "Local Desktop C" = hybrid C installed in the test slot app.polarbear (APK ld-20261029-C-overnight = exp/hybrid-c + slot commit with label "Local Desktop C", branch slot/abc-C), Plasma config (/root/toml-plasma), verified: KWin OpenGL ES on Zink/Turnip, 0 crashes. frame-stats switch is on in this slot (files/frame-stats; cb[...] lines in logcat). Control "Local Desktop" untouched. Upstream B is NOT on the phone now; `abc-next/restore-b.sh` puts it back. Next session first: `adb logcat -d -b crash`, the app's /tmp/plasma-mobile-session.log + /tmp/localdesktop-session.log in the slot, and the user's impressions.

## 2026-10-30 ~11:45 — A/B/C RESULTS (full run done) — read this first
User ran "Local Desktop C" (hybrid) overnight by hand: "seems to run well". Overnight check: 0 app crashes (only a ChatGPT crash at 05:17), Plasma log only the usual QuickSettings.qml TypeErrors; logs in ~/localdesktop-bench/C-overnight-20261030/.

Runs: abc-next/results-20261030-0843 (3 rounds × 7 configs) + results-xfce-20261030-1111 (Xfce rounds 1–2 re-run). 29 runs, 0 crashes.
- BUG found + fixed: ~/bin/ld-framebench and ld-shadejank pointed at the main checkout (control branch, no SHELL_PROC) → on Xfce, es2gears/Brave never launched (rounds 1–2 Xfce of the main run invalid: no fps, CPU 7%). Now ~/bin/ld-framebench + ld-shadejank → worktree ~/localdesktop-bench/tools-xfce-gpu (fix/xfce-gpu, detached; same behaviour on Plasma). Main checkout untouched. Script for Xfce-only runs: abc-next/run-abc-xfce.sh.
- Every adb install now needs an on-phone Play Protect confirmation (user approves; runs wait). Google MFA prompts at 11:08 and 11:23 hit cold starts only (B-xfce-r3 start 2, C-xfce-r1 start 1: excluded); no benchmark phase was interrupted.

| 3 rounds avg | Plasma SF fps | Plasma gears | Plasma Brave jank | Plasma CPU | Xfce SF fps | Xfce Brave jank | Xfce CPU |
|---|---|---|---|---|---|---|---|
| A control loop | 66.1 (57/69/73) | 61.6 | 14.1% | 134% | 38.7 | 12.9% | 152% |
| 3a upstream loop | 103.0 (105/106/98) | 111.2 | 13.6% | 147% | 25.4 | 13.5% | 174% |
| C hybrid | 101.0 (93/103/107) | 113.0 | 14.3% | 152% | 38.5 | 14.4% | 162% |
| B upstream #328 | (no Plasma) | | | | 24.8 | 14.3% | 163% |
Cold starts: Plasma ~7 s all, Xfce ~5 s all (no difference). CPU numbers are noisy (±20 points within a config).
Conclusions: C = 3a on Plasma (≈1.5× A) AND = A on Xfce (3a/B lose a third). On Xfce C releases on commit only (cb 0/0/452, like A); on Plasma mixed vsync/commit. Only cost seen: Brave+shade jank in C is ~0.7–1.5 points higher than the best loop on each desktop, in every round (small but consistent). → C is the leader; the jank gap is the one thing to look at before merging.
Phone: "Local Desktop B" = upstream #328 + Xfce (as on 10-29 evening); control "Local Desktop" untouched; stay_on restored to 0. C is no longer installed (APK ld-20261029-C-overnight, branch slot/abc-C, to put it back).
Next (user decides): merge C into the stack? look at C's jank first? merge fix/xfce-gpu? drop 3b?

## 2026-10-30 ~12:15 — HYBRID C IS THE NEW CONTROL (merged, single APK) — read this first
User: "Hybrid C ... will be the new control. It should be the only APK built moving forward, the only variant of these apps installed on the phone, and GitHub needs to be updated."
- Merged exp/hybrid-c into perf/gpu-compositing-pacing (merge a63ec7f, no-ff). Code identical to the tested C build (slot/abc-C differed only in manifest package/label). Brings 3a (496f074), hybrid (98a8303), frame-callback trace (3c86924) and fix/xfce-gpu (6cecc39, a9e9ad5, 19a30ff, 85a427b). 3b NOT merged (no gain).
- Design + full A/B/C results: docs/frame-loop.md (new); short section in docs/architecture.md; ROADMAP.md control + graphics sections updated.
- APK **ld-20261030-a-hybrid-control** (io.github.localdesktop, "Local Desktop"), in ~/localdesktop-bench/apks/ and control/. Installed over the old control with `adb install -r` (data + Steam kept). Verified 12:01: Plasma up, KWin up, build-id matches, ld-framebench SF 104 fps, es2gears 114–119, cb mixed vsync/commit, 0 crashes. frame-stats switch left OFF.
- Test slot app.polarbear UNINSTALLED (26 GB, a 10-29 copy of the control rootfs). Its unique files (toml-plasma, toml-xfce-ours, toml-xfce-up, ld-runner.sh) saved in ~/localdesktop-bench/abc-next/b-slot-files/. Phone now has only io.github.localdesktop.
- Tag **control-2026-10-30-hybrid-c**. Experiment branches stay local (bundle in ~/localdesktop-bench/control/).
- Caveat found while documenting: the Xfce "ours" test config ran apps with LIBGL_ALWAYS_SOFTWARE=1 MESA_VK_WSI_DEBUG=sw (+ labwc pixman), so es2gears on Xfce was probably on llvmpipe — the Xfce fps are a CPU-path ceiling (same for all loops, so the comparison holds). ld-framebench doesn't record the renderer: open item.
- Fresh-install Xfce default after the merge: labwc on the GPU (stalls on a fence) → needs WLR_RENDERER=pixman until fixed. Plasma (the phone's config) unaffected.
- ~/bin/ld-framebench + ld-shadejank point back at ~/localdesktop/scripts/device (main now has SHELL_PROC); the tools-xfce-gpu worktree is removed.
Next (user decides): C's shade jank (+~1 pt), labwc GPU fence stall, renderer in ld-framebench; Steam/Game Mode list in ROADMAP.

## 2026-10-30 ~12:40 — NEW PROJECT: keep running in the background (added to ROADMAP)
User: "Programs should continue to function if I minimize the app and use my phone for something else. A Browser playing a YouTube video should keep playing, a game should still be running (maybe paused?), but it shouldn't just close/crash."
Known: hidden → renderer dropped + no frame callbacks (clients stall); phantom process killer / battery optimization / memory pressure can kill proot children; session only starts in the foreground. Plan sketch in ROADMAP "Keep running in the background": foreground service, pacing while hidden, audio, clean resume; first build a background test tool. Not started; the user picks when.

## 2026-10-30 ~13:00 — GitHub: fork main = our work, 0 behind upstream
- User: fork should look "totally up to date with the commits on the main thread, plus the improvements made through the hybrid", with the current goals as a roadmap.
- 20acb22: ROADMAP "Keep running in the background" + README note linking ROADMAP.md and docs/frame-loop.md. 14ca74c: `git merge -s ours origin/main` (0543368) — tree unchanged (= tested control code), history now contains all upstream commits (the 9 after #328 were already cherry-picked; #328 superseded).
- Pushed 14ca74c to fork main (fast-forward from 0543368) and fork perf/gpu-compositing-pacing. GitHub compare upstream main...fork main: behind_by=0 ahead_by=75. The repo isn't registered as a GitHub fork (no parent), so GitHub shows no ahead/behind banner on its own.
- From now on push to fork BOTH main and perf/gpu-compositing-pacing (they are the same commit).

## 2026-10-30 ~13:10 — one branch: main
- fork main and perf/gpu-compositing-pacing were the same commit (14ca74c, 0/0 apart, no open PRs). Deleted perf/gpu-compositing-pacing on the fork and locally; local checkout is now `main`, tracking fork/main. Working branch from now on = main; push only main. (Older sections still say perf/gpu-compositing-pacing: read that as main.)

## 2026-10-30 ~13:30 — names simplified (no more control / hybrid C)
User: "it doesn't need to remain control hybrid C, that naming convention was for the purpose of the last round of testing."
- Docs: ROADMAP "Control baseline" → "Current state" (main is the app; short-lived branches merge back); frame-loop.md rewritten: current design first, the A/B/C labels only in the test record; architecture.md heading "Frame loop".
- Builds now use the repo's default BUILD_ID (dev-<commit>); no named control APKs.
- Backup of all branches + tags: ~/localdesktop-bench/control/all-branches-20261030.bundle (103 refs, verified).
- DONE 13:45 (user OK): deleted tags control-2026-10-29, -merged, -v2, control-2026-10-30-hybrid-c on fork + locally, stable-20261027 locally; delete the 16 local experiment/slot/test branches (all in the bundle). backup/* branches and tags stay.
- Installed dev-1aede1b (13:40): Plasma + KWin up, 0 crashes. (First install attempt failed: USB dropped; retried.)
- Repo now: branches main + backup/pre-agnostic-rewrite + backup/pre-email-rewrite (backups, never push); tags = upstream's v*/extension-* + local backup/* tags.

## 2026-10-30 ~13:55 — roadmap ORDER set by the user
1. Short fixes in order: Brave as default browser → CJK fonts → GL renderer in ld-framebench → DNS refresh. (Duplicate Brave drawer entry: disregard — only one Brave install.)
2. App process lifecycle: keep running in the background (+ guest-death handling).
3. Compositor: shade jank, labwc fence stall, then multi-display / notification bridge.
4. Steam and games: controller input, any resolution/aspect ratio, then the rest.
ROADMAP.md "Order of work" holds this; backlog is unordered.
