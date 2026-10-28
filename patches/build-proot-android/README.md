# build-proot-android

PRoot build scripts for Android. They produce PRoot binaries, statically linked with libtalloc, with unbundled loader and freely relocatable in a file tree.

Usage:
- Build or get prebuilt at https://github.com/green-green-avk/build-proot-android/tree/master/packages
- Unpack `<somewhere>`
- Run as `<somewhere>/root/bin/proot`\
for details, see https://github.com/green-green-avk/proot/blob/master/doc/usage/android/start-script-example
- ???
- Profit

How to build:
 - Dependencies: Android NDK / make / tar / gzip
 - Tune `config` file to match your environment
 - Run `./build.sh`

Vendored `build/proot` tracks the latest `termux/proot` `master` commit that
was current on March 31, 2026: `ab2e3464d04483b98a0614b470f3f8950d5a6468`
(committed on February 21, 2026). This repo still carries a small set of
downstream Android/APK build patches on top of that source snapshot.

See https://github.com/green-green-avk/proot for more info.

## Performance patches (2026-10-23)

`build/proot/src` now also carries the proot patches from
[DroidDeck](https://github.com/Droid-Deck/DroidDeck) (GPL-3.0, `tools/proot/patches`,
commit 8ebba02), most of them ported from WinNative. They are kept in `patches/` for reference,
already applied to the vendored source, in file-name order, on top of `ab2e3464`; see
`patches/PATCHES-droiddeck.md` for what each one does. Highlights: `ioctl` (every GPU
submit), `futex`, `uname` and most fd syscalls no longer stop in the tracer, and path
syscalls, `fork`/`exec` and thread creation stop less.

`patches/0018-localdesktop-keep-statx-exit-for-stat-fixups.patch` is ours. DroidDeck's
`0013` drops the `statx` exit stop on kernels 5.8 and newer, but `statx` is where proot calls
fake_id0 (`--root-id`) and link2symlink back to fix ownership and link counts, so without it
every file looked owned by the Android app uid. The stop is kept while such an extension is
loaded.

Build with `./build-patched.sh` (Android NDK r27c at `$NDK`, default `~/Android/android-ndk-r27c`).
It writes `libproot.so` and `libproot_loader.so`, 16 KB aligned, into `assets/libs/arm64-v8a/`.

`patches/0019-localdesktop-execveat-absolute-path.patch` is ours too. proot turned every
`execveat()` whose fd is not `AT_FDCWD` into `ENOSYS`, but the kernel ignores the fd when the
path is absolute (and `AT_EMPTY_PATH` is not set), so such a call is an `execve()`. FEX starts
every x86 child process as `execveat(<loader fd>, "/proc/self/exe", ...)`, so under proot no x86
program could start another (games' launcher scripts failed with "Function not implemented").
DroidDeck works around the same thing in a session preload (`robust.c`); here proot handles it.
