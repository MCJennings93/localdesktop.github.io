# Emulated Steam Deck controller

Steam on the phone needs a controller for Big Picture, Proton and gamescope work, and the phone
has none. Local Desktop emulates a Steam Deck controller the way DroidDeck does (GPL-3.0, at
4f58230a), with the same library.

## How it fits together

```
input source ──► ring file ──► libfakeinput.so in the Steam client ──► "/dev/hidraw16"
(localdesktop-pad      /run/localdesktop-pad/        serves a Steam Deck controller      Valve Software
 today; touch controls fakeinput-rings/ring0         (28de:1205, 64-byte reports         Steam Deck Controller
 later)                                              type 0x09 every 4 ms)                   │
                                                                                     Steam Input
                                                                                          │
games ◄── "/dev/input/event16+" ◄── libfakeinput in the game ◄── Steam's virtual pad via fake /dev/uinput
```

- **libfakeinput.so** (`guest/managed/usr/local/src/fakeinput_steam.cpp`, DroidDeck's
  `app/src/main/cpp/fakeinput_steam.cpp` unchanged; from WinNative) interposes libc (`open`,
  `ioctl`, `read`, `poll`, `stat`, `opendir`, `inotify`, ...). No kernel device is needed, which
  proot could not provide. It serves the Deck pad only to a process whose executable is named
  `steam`. `guest/build-fakeinput.sh` cross-builds it in Debian bullseye (glibc 2.31, static
  libstdc++): aarch64 for the client, x86-64 for games under FEX.
- **The ring** is a 98 688-byte file: a 64-byte header with a seqlocked snapshot (12 button bits,
  8 axes), 4096 evdev events, a 64-byte IMU and trackpad block and a 256-byte Steam Controller
  block, all little-endian. The Deck reports are built from the snapshot and the IMU block.
- **sysfs and udev**: Steam finds the pad through `/sys/class/hidraw/hidraw16` →
  `/sys/devices/droiddeck/usb1/1-1:1.2/0003:28DE:1205.0001/hidraw/hidraw16`, the USB and HID
  attributes and report descriptor there, `/sys/dev/char/240:16` and `/run/udev/data/c240:16`.
  The app binds `files/pad-sysfs/devices` over `/sys/devices/droiddeck` and `files/pad-sysfs/hidraw`
  over `/sys/class/hidraw` (`pad_sysfs` in `src/android/proot/process.rs`). That directory is
  beside the rootfs, not in it: proot names an open file by its rootfs path first, so
  `/proc/self/fd` would read back `/sys/.pad/...` and libudev would drop the device;
  `localdesktop-pad setup` (session start) writes the entries. `/sys/dev/char` is already the
  app's fake DRM sysfs. The names keep DroidDeck's (`droiddeck`, `DROIDDECK0001`) because the
  library has them built in.
- **localdesktop-steam** preloads the library into the client with the variables
  `localdesktop-pad env` prints.

## Differences from DroidDeck

- DroidDeck's app writes the ring from Kotlin (`FakeInputWriter`) and the sysfs tree from
  `SteamDeckPad.kt`. Here the guest does both (`localdesktop-pad`), and the app only binds two
  directories. Input sources write the same ring.
- Only the Deck pad is described so far: not the Steam touch controller (`0000:11fb`,
  `/dev/hidraw17`), the Steam Controller, DualShock 4 or DualSense that the library also emulates,
  nor the evdev sysfs (`InputSysfsComponent`) games use to find Steam's virtual pad.

## Testing

`localdesktop-pad press A`, `release`, `tap B 150`, `stick left 0.5 -1`, `trigger right 1`,
`dpad up`, `state`. The library logs to `/run/localdesktop-pad/pad.log`.

On the PC, the x86-64 build was checked end to end in Debian bullseye: a test program named
`steam` read `/dev/hidraw16` reports matching each `localdesktop-pad` state (A → `0x80`, left
trigger past half → L2 `0x02`, Quick Access → high `0x40000`, d-pad up → `0x100`, sticks with
the Deck's Y sign).

## Next

1. Phone: Steam lists a Steam Deck controller; buttons work in Big Picture.
2. Games: x86-64 libfakeinput in the FEX rootfs and the evdev sysfs, so Bastion sees Steam's
   virtual pad.
3. Touch controls drawn by the app's compositor, writing the ring; then Steam's touch controller
   (`0000:11fb`), whose layouts and per-game configs are Steam's.
