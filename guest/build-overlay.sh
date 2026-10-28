#!/bin/sh
# Packs the guest files the app installs into the Arch rootfs (see setup_guest_overlay in
# src/android/proot/setup.rs) as APK assets:
#   assets/guest-overlay.tar  managed/ (rewritten on every app start) and seed/ (written only
#                             if missing, so user edits stick), plus turnip-kgsl.sha256
#   assets/turnip-kgsl.tar.xz Mesa Turnip with the KGSL backend for /opt/turnip-kgsl, unpacked
#                             again whenever its sha256 changes
# BUILD_ID names the build (default dev-<commit>); it lands in /etc/localdesktop/build-id so
# tests on the device can record which APK they ran on.
# Both are build outputs (gitignored); `make build-docker` runs this first.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ASSETS=$HERE/../assets
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp "$HERE/blobs/turnip-kgsl.tar.xz" "$ASSETS/turnip-kgsl.tar.xz"
sha256sum "$HERE/blobs/turnip-kgsl.tar.xz" | cut -d' ' -f1 > "$TMP/turnip-kgsl.sha256"
cp -a "$HERE/managed" "$HERE/seed" "$TMP/"
# Packages localdesktop-packages installs at session start (guest/blobs/packages).
mkdir -p "$TMP/managed/usr/local/share/localdesktop/packages"
cp "$HERE"/blobs/packages/*.pkg.tar.* "$TMP/managed/usr/local/share/localdesktop/packages/"
BUILD_ID=${BUILD_ID:-dev-$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)}
mkdir -p "$TMP/managed/etc/localdesktop"
echo "$BUILD_ID" > "$TMP/managed/etc/localdesktop/build-id"
# Modes come from here, not from the checkout: programs and shims 0755, everything else 0644.
find "$TMP/managed" "$TMP/seed" -type d -exec chmod 755 {} +
find "$TMP/managed" "$TMP/seed" -type f -exec chmod 644 {} +
find "$TMP/managed/usr/local/bin" "$TMP/managed/usr/local/lib" -type f -exec chmod 755 {} +
chmod 755 "$TMP/seed/root/.config/labwc-plasma/autostart"
# Reproducible: fixed owner, order and timestamps, so an unchanged guest/ gives the same asset.
tar -C "$TMP" --owner=0 --group=0 --numeric-owner --sort=name --mtime=@0 \
    -cf "$ASSETS/guest-overlay.tar" managed seed turnip-kgsl.sha256
echo "guest-overlay.tar ($BUILD_ID): $(tar -tf "$ASSETS/guest-overlay.tar" | grep -vc '/$') files"
