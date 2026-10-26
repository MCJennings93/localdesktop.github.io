# Device test tools

Shell tools for working on Local Desktop over adb, from a PC, against a phone running the app. They
were written against one test phone, a foldable: screenshots use its main display
(`screencap -d 4630947175568309891`) and taps assume its inner-screen coordinates, so adjust those
for another device. Results go to `~/localdesktop-bench/`.

| Tool | What it does |
|---|---|
| `ld-exec 'cmd'` | Runs a command in the Arch guest under `run-as`, as the app's proot does. No GPU (the `run-as` SELinux domain may not use it) and no network. |
| `ld-run 'cmd' [timeout]` | Runs a command inside the live session, in the app's own SELinux domain and with the session's environment, so the GPU works. Needs `/etc/localdesktop/test-runner` in the guest. |
| `ld-pacman pkg...` | Installs guest packages: downloads them (with signatures) on the PC and installs offline, since `pacman -S` via `ld-exec` has no network. |
| `ld-framebench [label]` | Frame-timing benchmark: idle, app-drawer animations, es2gears. SurfaceFlinger timestats, the app's frame-stats log, and CPU of the app, KWin and plasmashell. Needs `files/frame-stats` at app start. |
| `ld-abrun <build-id>` | Installs `~/localdesktop-bench/apks/<build-id>.apk`, checks `/etc/localdesktop/build-id` and the shell, then runs `ld-framebench` twice. For A/B tests between builds made with `make build-docker BUILD_ID=<build-id>`. |
| `ld-synctest <label> [secs] [iterations] [passes] [env...]` | Runs `guest/tools/synctest.c` (built as `/root/synctest/synctest` in the guest) and sums the frame-stats `sync[...]` counters: partly drawn (`mixed`) and `stale` frames reaching the screen. |
| `ld-greentest <label> [secs] [flags]` | Plays a grayscale test video (or `URL=`, e.g. YouTube) in Brave and counts green frames, i.e. video frames shown before they were decoded into. Options in the script header. |
| `ld-shadetest <label> [cycles] [flags]` | Opens and closes Android's quick-settings shade over Brave and screenshots the screen after each close, counting garbage. The pass/fail test for GPU-buffer hand-off sync with Chromium. `VIDEO=1`, `URL=`, `PROFILE_SRC=` play a video, open another page or use another profile; it also counts frame-stats GREEN frames. |

Every tool records the device's build ID next to its results.
