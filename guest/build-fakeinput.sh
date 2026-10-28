#!/bin/sh
# Cross-builds guest/managed/usr/local/src/fakeinput_steam.cpp (DroidDeck's libfakeinput) in Debian
# bullseye, as DroidDeck's tools/build_local.sh does: its glibc 2.31 keeps the library loadable in newer
# guests, and libstdc++ is linked in statically.
#   usr/local/lib/libfakeinput.so                         aarch64, preloaded into the Steam client
#   usr/local/lib/localdesktop-fex/x86_64/libfakeinput.so for x86 games under FEX
# The source goes in on stdin and the libraries come back as a tar stream: no bind mount, which
# SELinux (Fedora) would refuse without relabelling the checkout.
set -eu
cd "$(dirname "$0")/managed/usr/local"
docker run --rm -i --platform linux/amd64 public.ecr.aws/docker/library/debian:bullseye sh -euc '
    cat > /tmp/fakeinput_steam.cpp
    {
        printf "deb http://archive.debian.org/debian bullseye main\n"
        printf "deb http://archive.debian.org/debian-security bullseye-security main\n"
    } > /etc/apt/sources.list
    apt-get -o Acquire::Check-Valid-Until=false update -qq >&2
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        g++ g++-aarch64-linux-gnu binutils-aarch64-linux-gnu >&2
    flags="-shared -fPIC -O2 -Wall -Wno-attributes -Wno-nonnull-compare -pthread -std=c++17"
    flags="$flags -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL"
    mkdir -p /out/lib/localdesktop-fex/x86_64
    aarch64-linux-gnu-g++ $flags -o /out/lib/libfakeinput.so /tmp/fakeinput_steam.cpp -ldl >&2
    aarch64-linux-gnu-strip --strip-unneeded /out/lib/libfakeinput.so
    g++ $flags -o /out/lib/localdesktop-fex/x86_64/libfakeinput.so /tmp/fakeinput_steam.cpp -ldl >&2
    strip --strip-unneeded /out/lib/localdesktop-fex/x86_64/libfakeinput.so
    tar -C /out -c lib' < src/fakeinput_steam.cpp | tar -x --no-same-owner
