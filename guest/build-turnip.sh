#!/bin/sh
# Builds guest/blobs/turnip-kgsl.tar.xz (Mesa's Turnip with the KGSL backend, installed at
# /opt/turnip-kgsl) on the phone, inside the live session (which has the network): the release
# tarball, sha256-checked, with patches/mesa/*.patch applied in name order.
# Usage: build-turnip.sh   (takes a while: a full Turnip build on the phone)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=26.2.3
SHA256=1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f
STAGE=/root/build/turnip
adb shell "run-as app.polarbear sh -c 'rm -rf files/arch$STAGE && mkdir -p files/arch$STAGE/patches'"
for f in "$HERE"/../patches/mesa/*.patch; do
    adb shell "run-as app.polarbear sh -c 'cat > files/arch$STAGE/patches/$(basename "$f")'" < "$f"
done
"$HERE/../scripts/device/ld-run" "set -e; cd $STAGE
curl -fsSL -o mesa.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo '$SHA256  mesa.tar.xz' | sha256sum -c --quiet
tar -xf mesa.tar.xz
cd mesa-$VERSION
for p in ../patches/*.patch; do patch -p1 --no-backup-if-mismatch < \"\$p\"; done
meson setup build -Dbuildtype=release -Dprefix=/opt/turnip-kgsl -Dlibdir=lib -Db_ndebug=true \
    -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm -Dgallium-drivers= -Dplatforms=wayland,x11 \
    -Dopengl=false -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dvideo-codecs= -Dvulkan-layers= -Dtools= > ../meson.log 2>&1 || { tail -20 ../meson.log; exit 1; }
DESTDIR=$STAGE/root ninja -C build install > ../ninja.log 2>&1 || { tail -30 ../ninja.log; exit 1; }
tar -C $STAGE/root --owner=0 --group=0 --numeric-owner --sort=name --mtime=@0 -cJf $STAGE/turnip-kgsl.tar.xz opt/turnip-kgsl
echo built" 7200 | tee /dev/stderr | grep -q '^built$'
adb shell "run-as app.polarbear cat files/arch$STAGE/turnip-kgsl.tar.xz" > "$HERE/blobs/turnip-kgsl.tar.xz"
echo "guest/blobs/turnip-kgsl.tar.xz: $(sha256sum "$HERE/blobs/turnip-kgsl.tar.xz" | cut -d' ' -f1)"
