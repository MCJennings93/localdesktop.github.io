#!/bin/sh
# Builds the host-side (Android, bionic) probes in this folder with the NDK, and pushes them to
# /data/local/tmp. Usage: tools/android/build.sh <name> (e.g. memobjprobe).
set -eu
NDK=${NDK:-$HOME/Android/android-ndk-r27c}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang
D=$(dirname "$0")
$CC -O2 -Wall -o "$D/$1" "$D/$1.c" -lEGL -lGLESv3 -lvulkan -landroid
adb push "$D/$1" /data/local/tmp/ >/dev/null
