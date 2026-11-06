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

**Games.** Steam Input hands a game its virtual pad, created through libfakeinput's fake
`/dev/uinput` as `/dev/input/event16` and up, and the x86-64 libfakeinput (preloaded into games
under FEX by `localdesktop-fex`) serves it. A newer SDL scans `/dev/input` (Steam sets
`SDL_JOYSTICK_DISABLE_UDEV=1`). An SDL before 2.0.14 (Bastion's) finds pads only through libudev,
and Android refuses apps udev's netlink socket, so SDL dropped joystick support altogether. For it:
- `udevmon.c` (linked into libfakeinput, from DroidDeck) turns the refused netlink socket into a
  Unix datagram pair answering bind, getsockname, setsockopt and recvmsg like netlink, and injects
  add/remove messages (with `ID_INPUT_JOYSTICK`) when a virtual pad's node appears or goes.
- libfakeinput writes each virtual pad's `/sys/devices/virtual/input/inputN/eventN` (capability
  bitmaps from the uinput description), `/sys/class/input` links and `/run/udev/data` records
  before the node appears, and removes them after. The app binds `files/pad-sysfs/input-devices`
  and `input-class` over those sysfs directories. `localdesktop-pad setup` clears entries left by
  a Steam that died.
- libfakeinput's `/sys/class` listing stand-in now serves every process, not only the client.

**Slots: each controller as itself.** The guest has one ring per controller slot
(`/run/localdesktop-pad/fakeinput-rings/ring0` to `ring3`, all named in `FAKE_EVDEV_MEMFD_PATHS`):

- **Slot 0** is the Steam Deck controller above: the on-screen gamepad's, and that of any controller
  without a slot of its own (today every non-Nintendo one), merged by position. Steam sees it only
  while it is in use, the on-screen gamepad shown or such a controller connected; otherwise the app
  sets slot 0's pad type to "none" and libfakeinput takes the Deck away (a udev `remove`, no
  `/sys/class/hidraw` link), so Steam lists only the real controllers.
- **Slots 1 to 3** each carry one physical controller paired with Android, presented as itself on
  `/dev/hidraw33` to `35`, so each is a player of its own in Steam and in games (Steam Input makes
  each its own virtual pad), with its own glyphs, templates and community layouts. Today a Nintendo
  controller gets one, as a Switch Pro Controller.

`ControllerMotion.java` reports controllers connecting and going away to
`src/android/app/controller_slots.rs`, which gives slots and writes each slot's pad type (bit 31 set)
into its ring's IMU block, and writes them again twice a second so a new session's rings get them.
A device counts as a controller only with the whole `SOURCE_GAMEPAD` or `SOURCE_JOYSTICK` flag: they
share class bits with keyboards, so a plain mask also matches the phone's touchpad and screen.
`gamepad.rs` writes a slotted controller's buttons and axes into its own ring. In the Steam client,
libfakeinput reads the slots every 50 ms; when one gains a controller it describes its hidraw node in
sysfs (`/sys/devices/droiddeck/slotN/…`, the `/sys/class/hidraw` link, `/sys/dev/char`, the udev
record) and then sends Steam's udev monitor an `add`, and the other way round when it goes. Slots
1 to 3 are never evdev pads (`/dev/input/event1` to `3`).

**Switch Pro Controller.** libfakeinput speaks Nintendo's Bluetooth HID protocol (`0005:057E:2009`):
full input reports 0x30 every 8 ms (buttons by label, 12-bit sticks, the slot's IMU block), replies
0x21 to subcommands (device info, SPI flash reads of factory stick and IMU calibrations and colours,
plain acks for modes and lights) and rumble from reports 0x01/0x10. Like the kernel's hidraw, it
never lets old input reports block what comes next: it skips an input report while a reply waits or
while half the socket is unread (Steam stops reading for seconds while it sets up its virtual pads),
so every reply gets through. Capture tapped alone is Home; Capture + Minus is the Switch's Capture
button (Android keeps Home for itself). The same type can also be slot 0's, with
`FAKE_EVDEV_PAD_TYPE=switchpro` (`/dev/hidraw22`, described by `localdesktop-pad setup`).

**Gyro.** `ControllerMotion.java` reads the gyro and accelerometer of the controller connected last
that has them, through `InputDevice.getSensorManager()` (Android 12+), while Steam wants motion
from it (the IMU mode Steam sets on that slot's controller, which libfakeinput writes into the IMU
block). `src/android/app/controller_motion.rs` writes each reading into the IMU block of that
controller's slot, in the Deck's axes (x, -z, y of the controller's x right, y up, z toward the
player) and units; the Switch Pro's reports convert them back to Nintendo's. `ld-gyrotest` checks
it with a virtual DualShock 4 made by Android's `hid` tool.

**Rumble** from Steam goes to the abstract socket `droiddeck-rumble`, which only DroidDeck's app
listens on: controller rumble does not reach Android yet.

A controller paired with Android reaches the app as winit device events (axes and buttons), so it
no longer produces arrow keys or mouse motion. Mode (Capture on a Switch Pro) tapped alone is the
Steam button for 80 ms, Mode + Select (Minus) is Quick Access, and Mode + another button is Steam
plus that button.

`ld-padtest` (`scripts/device`) needs no physical controller. Android's `uinput` tool creates a
virtual one with a Switch Pro Controller's ids, so Android applies its real key layout, and the
test reads the rings (`localdesktop-pad --slot N state`). It checks that the controller gets slot 1
as a Switch Pro and leaves it when unplugged, that slot 0 stays clear meanwhile, all buttons,
triggers, the d-pad, sticks, chords, the Steam tap and unplugging with a button down; then that a
virtual Xbox pad, with no slot of its own, brings the Deck back on slot 0. That covers Android to
the rings; Steam's side shows in `pad.log` (`slot 1: a Pro Controller on /dev/hidraw33`).

## Next

1. Gyro: try gyro as mouse in a game's layout. Steam already follows the gyro (Settings →
   Controller → Calibration & Advanced Settings → gyro calibration, with `ld-gyrotest --wave`).
2. Sony controllers on slots of their own as a DualShock 4 or DualSense (libfakeinput has both;
   their slot sysfs needs USB parents), and other controllers as Xbox pads.
3. Rumble from Steam to the controller (Android's vibrator for it).
4. Touch controls drawn by the app's compositor, writing the ring; then Steam's touch controller
   (`0000:11fb`), whose layouts and per-game configs are Steam's.

## Requirements for the on-screen gamepad

- One quick toggle shows or hides it (no settings page), and the choice persists.
- With a physical controller connected it stays hidden unless turned on, while touches still go to
  the game (as touch/mouse): no buttons drawn over the picture.
- Hidden means the touches it would have taken go to the game instead.
