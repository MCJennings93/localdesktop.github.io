#!/usr/bin/env bash
# Build proot from a patched source tree with the NDK, as DroidDeck's tools/proot/build.sh does.
# Usage: build-patched.sh [outdir]  (default: ../../assets/libs/arm64-v8a)
# Builds the vendored, already-patched build/proot with talloc linked in statically.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/build/proot; TALLOC=$HERE/build/talloc-2.4.3
OUT=$(mkdir -p "${1:-$HERE/../../assets/libs/arm64-v8a}" && cd "${1:-$HERE/../../assets/libs/arm64-v8a}" && pwd)
NDK=${NDK:-$HOME/Android/android-ndk-r27c}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC=$TC/aarch64-linux-android26-clang
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
cp -a "$SRC" "$WORK/proot"
mkdir -p "$WORK/talloc/config"
cp -a "$TALLOC"/. "$WORK/talloc/"
cp "$HERE/patches/talloc-config.h" "$WORK/talloc/config/config.h"
"$CC" -c -O2 -fPIC -ffile-prefix-map="$WORK"=. -D__STDC_WANT_LIB_EXT1__=1 -DHAVE_CONFIG_H \
  -I"$WORK/talloc/config" -I"$WORK/talloc" -I"$WORK/talloc/lib/replace" \
  -o "$WORK/talloc.o" "$WORK/talloc/talloc.c"
"$TC/llvm-ar" rcs "$WORK/libtalloc.a" "$WORK/talloc.o"
mkdir -p "$WORK/bin"; ln -s "$TC/llvm-readelf" "$WORK/bin/readelf"
PATH="$WORK/bin:$PATH" make -C "$WORK/proot/src" -j"$(nproc)" \
  CC="$CC" STRIP="$TC/llvm-strip" OBJCOPY="$TC/llvm-objcopy" OBJDUMP="$TC/llvm-objdump" \
  CPPFLAGS="-ffile-prefix-map=$WORK=. -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I. -I$WORK/talloc -DARG_MAX=131072 -Wno-error=implicit-function-declaration" \
  LDFLAGS="$WORK/libtalloc.a -Wl,-z,noexecstack -Wl,-z,max-page-size=16384" \
  PROOT_UNBUNDLE_LOADER=/nonexistent proot loader >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; exit 1; }
grep -q '^#define HAVE_SECCOMP_FILTER' "$WORK/proot/src/build.h" || { echo "no seccomp"; exit 1; }
grep -q '^#define HAVE_PROCESS_VM' "$WORK/proot/src/build.h" || { echo "no process_vm"; exit 1; }
install -m755 "$WORK/proot/src/proot" "$OUT/libproot.so"
install -m755 "$WORK/proot/src/loader/loader" "$OUT/libproot_loader.so"
"$TC/llvm-strip" --strip-unneeded "$OUT/libproot.so" "$OUT/libproot_loader.so"
"$TC/llvm-readelf" -d "$OUT/libproot.so" | grep NEEDED
"$TC/llvm-readelf" -lW "$OUT/libproot.so" | grep LOAD
ls -l "$OUT"/*.so
