#!/bin/sh
# Builds the guest's LD_PRELOAD shims (managed/usr/local/src/*.c) with the guest's own gcc and
# glibc, so the .so files match the Arch Linux ARM userland they load into, and copies them to
# managed/usr/local/lib/. Needs a phone with Local Desktop set up (base-devel installed) on adb.
# Run from anywhere; then run build-overlay.sh.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/managed/usr/local/src
LIB=$HERE/managed/usr/local/lib
APK=$(adb shell pm path app.polarbear | tr -d '\r' | sed 's/^package://')
NATIVE=$(dirname "$APK")/lib/arm64
D=/data/data/app.polarbear/files
R=$D/arch
STAGE=/tmp/localdesktop-shims

in_guest() {
    adb shell run-as app.polarbear sh -c "'PROOT_LOADER=$NATIVE/libproot_loader.so PROOT_TMP_DIR=$D $NATIVE/libproot.so -r $R -L --link2symlink --root-id -w / --bind=/dev --bind=/proc --bind=/sys /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin sh -c \"$1\"'"
}

adb shell "run-as app.polarbear sh -c 'rm -rf $R$STAGE && mkdir -p $R$STAGE'"
for c in "$SRC"/*.c; do
    adb shell "run-as app.polarbear sh -c 'cat > $R$STAGE/$(basename "$c")'" < "$c"
done
# The loop runs from a script file: nested adb/run-as/proot quoting would expand its variables early.
printf '%s\n' 'set -e; cd "$(dirname "$0")"; for c in *.c; do gcc -O2 -Wall -fPIC -shared -o "${c%.c}.so" "$c" -ldl -lpthread; done' |
    adb shell "run-as app.polarbear sh -c 'cat > $R$STAGE/build.sh'"
in_guest "sh $STAGE/build.sh"
mkdir -p "$LIB"
for c in "$SRC"/*.c; do
    name=$(basename "$c" .c)
    adb exec-out "run-as app.polarbear cat $R$STAGE/$name.so" > "$LIB/$name.so"
    [ -s "$LIB/$name.so" ] || { echo "build-shims: $name.so is empty" >&2; exit 1; }
    echo "built $name.so ($(wc -c < "$LIB/$name.so") bytes)"
done
