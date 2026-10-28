#!/bin/sh
# Builds a package from guest/pkgbuilds/<name> on the phone, inside the live session (which has the
# network), and installs it with pacman -U. makepkg refuses to run as root and proot makes everything
# root, so a copy of makepkg without that check is used. glib2-devel's tools are unpacked into the
# build folder and put on PATH (installing glib2-devel would need a newer glib2 than the guest has).
# Usage: build.sh <name> [glib2-devel package file]
set -eu
NAME=$1; GLIBDEV=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
STAGE=/root/build/$NAME
adb shell "run-as app.polarbear sh -c 'rm -rf files/arch$STAGE && mkdir -p files/arch$STAGE'"
for f in "$HERE/$NAME"/* $GLIBDEV; do
    adb push -q "$f" /data/local/tmp/ld-build-file
    adb shell "chmod a+r /data/local/tmp/ld-build-file; run-as app.polarbear sh -c 'cp /data/local/tmp/ld-build-file files/arch$STAGE/$(basename "$f")'"
done
adb shell rm -f /data/local/tmp/ld-build-file
# The makepkg copy lives only in the build folder and is deleted when the build ends, whatever the
# result; /usr/bin/makepkg is never changed. Under proot "root" is a fake id with no power outside
# the guest's files.
"$HERE/../../scripts/device/ld-run" "set -e; cd $STAGE
trap 'rm -f $STAGE/makepkg-asroot' EXIT
sed 's/^\tif (( EUID == 0 )); then$/\tif false; then/' /usr/bin/makepkg > makepkg-asroot
for p in glib2-devel-*.pkg.tar.*; do [ -f \"\$p\" ] && mkdir -p glibdev && bsdtar -xf \"\$p\" -C glibdev usr/bin; done
[ -d glibdev ] && export PATH=\$PWD/glibdev/usr/bin:\$PATH
MAKEFLAGS=-j\$(nproc) bash makepkg-asroot -f --nodeps --noconfirm > makepkg.log 2>&1 || { tail -30 makepkg.log; exit 1; }
rm -f makepkg-asroot
tail -3 makepkg.log
pacman -U --noconfirm --noprogressbar ./$NAME-*.pkg.tar.* 2>&1 | tail -3" 3600
