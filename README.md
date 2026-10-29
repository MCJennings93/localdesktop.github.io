# Local Desktop

Local Desktop helps you run a desktop Linux environment on your Android device.

**Note**: It is expected that you already have a usable desktop experience, i.e., a large enough display (tablet or DEX), a physical keyboard, and optionally a mouse/trackpad. **Local Desktop aims to bridge the gap between the two platforms, not trying to "simulate" the desktop experience by introducing inconvenient interactions**.

## How it works

1. An Arch Linux ARM64 filesystem is set up inside the app's internal storage.
2. Proot mounts the filesystem and provides a chroot-like environment.
3. A minimal built-in Wayland compositor runs in Android NDK.
4. An Xfce Wayland session launches inside the chroot and renders back to the Android native activity.

![POC](./gh-pages/static/img/proof-of-concept.png)
_Proof of Concept: A Pixel Tablet running the XFCE desktop environment inside a Proot-based ARM64 Linux. No configuration is required; just install and go._

## Getting Started

### How to build an APK

#### Termux

You can build the Local Desktop APK directly on your Android device. This is simple because no cross-compilation is needed. However, we only ship prebuilt libs for arm64, so this option only works on ARM64.

For Termux, there are two supported approaches:

1. **Simple build (no `xbuild`)**

   Install Rust:

   ```
   pkg i rust
   ```

   Then run:

   ```bash
   cargo run
   ```

   This uses the built-in Rust APK builder and writes `localdesktop.apk` in the project root.

2. **`xbuild`-based build**

   This matches the desktop cross-build pipeline more closely and is better for repeated APK builds:

   ```bash
   bash scripts/build-termux.sh
   ```

   This script installs the required Termux packages, bootstraps the patched `xbuild` toolchain, downloads the Android SDK build-tools, and writes the APK to `target/x/release/android/localdesktop.apk`.

For the simple `cargo run` flow, you can install the APK with `termux-open localdesktop.apk`, but in some cases you may need to `mv localdesktop.apk ~/storage/downloads` and install it from there. Make sure you have run `termux-setup-storage` before moving it outside Termux.

#### Linux/Mac/Windows

The above option is suitable for quick development on Android. However, if you need to build an AAB (for distribution to Google Play), debug with Visual Studio Code, or compile for unsupported targets (like x64 Android), then you need to cross-build it from your PC.
Along with Rust, please make sure you have these components installed:

```
brew install llvm lld gradle@8
```

Then install our local version of xbuild:

```bash
cargo install --path patches/xbuild/xbuild --force
x build --release --platform android --arch arm64 --format apk
```

Alternatively, trigger the default build task by pressing `Ctrl+Shift+B`.

You can find the output APK in:

```bash
open target/x/release/android/gradle/app/build/outputs/apk/debug/app-debug.apk
```

#### Docker

If you have issues setting up your local development environment as detailed above, you can make use of the included Dockerfile to build an APK on a single command. This will take a long time so it's not recommended if you can make use of any of the other methods

```bash
make build-docker
```

You can find the output APK in:

```bash
open target/x/release/android/localdesktop.apk
```

### How to develop

#### Termux

We recommend the following setup, but feel free to use your own favorite tools for development:

```
pkg install helix helix-grammars rust-analyzer
```

A wonderful thing about developing directly on Android is that an agent like [Codex on Termux](https://github.com/DioNanos/codex-termux) can **test the code for you** by running unit tests, reading the output, understanding what went wrong, and trying again. In my experience, this is not possible when cross-developing on other operating systems, so you have to run the tests yourself.

![Developing on Android](./gh-pages/static/img/dev-from-android.webp)


#### Linux/Mac/Windows

Recommended setup:

- **IDE**: Visual Studio Code

- **Extensions**:

  - [Rust Analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer)
  - Local Desktop Dev, the project's own extension in [`vscode-extension/`](./vscode-extension)
  - [CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb) 1.11.4 or newer

- **Instructions**:

  1. Open Visual Studio Code.
  2. Press <kbd>F5</kbd> (the `Local Desktop: Debug on Android` configuration). This will:
     - Build the Rust code into an APK.
     - Install the APK on the selected device (USB or Wi‑Fi), and launch the app with LLDB attached.
     - Mirror the app's stdout/stderr into the Debug Console.

![It is easy to debug](./gh-pages/static/img/debugable.webp)

> **Tip**: You can debug the app on either a physical device or a virtual device.

For more instructions on how to work on this project, please visit the [Developer Manual](https://localdesktop.github.io/docs/developer/how-it-works).

### Guest files shipped in the APK

`guest/` holds files the app installs into the Arch rootfs at every start (`setup_guest_overlay` in `src/android/proot/setup.rs`):

- `guest/managed/`: rewritten whenever they differ from the APK's copy, so app updates reach them: the Plasma Mobile session scripts, the `LD_PRELOAD` shims (`usr/local/lib`, with sources in `usr/local/src`) and `localdesktop-install-plasma-mobile`, which installs the KDE package set and switches the session to Plasma Mobile.
- `guest/seed/`: written only when missing, so user edits stick: KDE look, Plasma Mobile and labwc settings, Brave's flags.
- `guest/blobs/turnip-kgsl.tar.xz`: Mesa Turnip built with the KGSL backend (the Adreno Vulkan driver behind Zink), unpacked into `/opt/turnip-kgsl` whenever the shipped build changes.
- `guest/tools/`: test programs that are not shipped (see `scripts/device/README.md`).

`guest/build-overlay.sh` packs these into `assets/` and runs as part of `make build` and `make build-docker`. `BUILD_ID=<name>` labels a build; the label lands in `/etc/localdesktop/build-id`. After editing a shim's source, `guest/build-shims.sh` rebuilds the `.so` files with the guest's own compiler over adb. To try edited guest files on a device without a rebuild, create `/etc/localdesktop/dev-keep-managed` so the app leaves the managed files alone, and delete it afterwards.

`kgsl-syncobj.so` emulates DRM sync objects on the KGSL GPU so KWin and Chromium-based apps use explicit sync (`wp_linux_drm_syncobj`) for the GPU buffers they exchange; `/etc/localdesktop/explicit-sync` turns it off or changes which programs are told a new enough kernel version.

The app also writes `/etc/resolv.conf` at every start from Android's active network (create `/etc/localdesktop/keep-resolv-conf` to manage it yourself).

## Credits

- The app icon is Tux by Larry Ewing (lewing@isc.tamu.edu), created with The GIMP.
- The proot performance patches (`patches/build-proot-android/patches/`), the KGSL render node setup in `src/android/proot/process.rs` and the guest's KGSL DRM shim follow [DroidDeck](https://github.com/Droid-Deck/DroidDeck) (GPL-3.0), much of whose proot work was ported from WinNative.
