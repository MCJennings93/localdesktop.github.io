#!/bin/sh
# Cross-builds the input method that opens the Android keyboard for KWin
# (managed/usr/local/src/android-keyboard/) in Debian bullseye, as build-fakeinput.sh does: its glibc
# 2.31 keeps the program loadable in newer guests. It links the guest's libwayland-client.
#   usr/local/libexec/localdesktop-android-keyboard  aarch64
# HOST_ARCH=1 builds it for this machine instead, into $OUT (default: the scratch dir printed), for
# testing against a nested kwin_wayland.
# Sources go in on stdin and the program comes back on stdout, as tar streams (no bind mount).
set -eu
cd "$(dirname "$0")/managed/usr/local"
if [ "${HOST_ARCH:-0}" = 1 ]; then
    cc=gcc arch=amd64 out=${OUT:-$(mktemp -d)}
else
    cc=aarch64-linux-gnu-gcc arch=arm64 out=libexec
fi
mkdir -p "$out"
tar -C src/android-keyboard -c . | docker run --rm -i --platform linux/amd64 public.ecr.aws/docker/library/debian:bullseye sh -euc '
    cc=$1 arch=$2
    mkdir -p /tmp/src && tar -x -C /tmp/src
    {
        printf "deb http://archive.debian.org/debian bullseye main\n"
        printf "deb http://archive.debian.org/debian-security bullseye-security main\n"
    } > /etc/apt/sources.list
    [ "$arch" = amd64 ] || dpkg --add-architecture "$arch"
    apt-get -o Acquire::Check-Valid-Until=false update -qq >&2
    pkgs="libwayland-dev:$arch libwayland-bin wayland-protocols"
    [ "$arch" = amd64 ] && pkgs="$pkgs gcc libc6-dev" || pkgs="$pkgs gcc-aarch64-linux-gnu libc6-dev-arm64-cross"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $pkgs >&2
    xml=/usr/share/wayland-protocols/unstable/input-method/input-method-unstable-v1.xml
    cd /tmp/src
    wayland-scanner client-header "$xml" input-method-unstable-v1-client-protocol.h
    wayland-scanner private-code "$xml" input-method-unstable-v1-protocol.c
    mkdir -p /out
    $cc -O2 -Wall -Wextra -Wno-unused-parameter -I. -o /out/localdesktop-android-keyboard \
        localdesktop-android-keyboard.c input-method-unstable-v1-protocol.c -lwayland-client >&2
    tar -C /out -c localdesktop-android-keyboard
' sh "$cc" "$arch" | tar -x -C "$out"
echo "built $out/localdesktop-android-keyboard ($(wc -c < "$out/localdesktop-android-keyboard") bytes)"
