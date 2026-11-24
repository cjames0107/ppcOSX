#!/bin/bash
# Build the QEMU fork for the distributable ppcOSX app: universal (arm64 and
# x86_64), macOS 14 and later, against the libraries tools/build-dist-deps.sh
# made (and macOS's own), with all of QEMU's optional features the app
# uses: TLS (gnutls), SSH and HTTPS images (libssh, curl), USB passthrough
# (libusb), VNC with JPEG, PNG screenshots, zstd, capstone.
#
#   tools/build-dist-qemu.sh                configure (first time) and build
#   ARCHS=arm64 tools/build-dist-qemu.sh    one architecture only
#
# Each architecture is built in dist/qemu-build-ARCH against
# dist/prefix-ARCH (x86_64 cross-compiled: QEMU's configure takes
# --cpu=x86_64 --cross-prefix= with -arch x86_64 in every compiler and
# linker flag). The
# binaries are merged with lipo into dist/qemu-build, which
# tools/bundle-emulator.sh takes (QEMU_BUILD=dist/qemu-build), with the
# icons QEMU installs beside them.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D="$ROOT/dist"
OUT="$D/qemu-build"
ARCHS=${ARCHS:-arm64 x86_64}
BINARIES="qemu-system-ppc qemu-system-ppc64 qemu-img"

export MACOSX_DEPLOYMENT_TARGET=14.0
unset PKG_CONFIG_PATH CPATH LIBRARY_PATH

for ARCH in $ARCHS; do
    PREFIX="$D/prefix-$ARCH"
    B="$D/qemu-build-$ARCH"
    FLAGS="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
    # x86_64 is a cross build: configure writes Meson's host machine only
    # with a cross prefix (empty here: Apple's compilers take -arch)
    CROSS="--cpu=aarch64 --cc=cc --cxx=c++ --objcc=clang"
    [ "$ARCH" = x86_64 ] && CROSS="--cpu=x86_64 --cross-prefix= --cc=cc --cxx=c++ --objcc=clang"
    if [ ! -f "$B/build.ninja" ]; then
        mkdir -p "$B"
        (cd "$B" && "$ROOT/qemu/configure" $CROSS \
            --target-list=ppc-softmmu,ppc64-softmmu \
            --enable-cocoa --enable-slirp --enable-tools \
            --enable-gnutls --enable-nettle --enable-libssh --enable-curl \
            --enable-libusb --enable-vnc --enable-vnc-jpeg --enable-png \
            --enable-zstd --enable-capstone --enable-coreaudio \
            --disable-docs --disable-werror --disable-guest-agent \
            --disable-sdl --disable-gtk --disable-spice --disable-curses \
            --disable-libiscsi --disable-libnfs --disable-lzo --disable-snappy \
            --disable-bzip2 --disable-auth-pam --disable-gcrypt \
            --extra-cflags="$FLAGS -I$PREFIX/include" \
            --extra-cxxflags="$FLAGS" \
            --extra-objcflags="$FLAGS" \
            --extra-ldflags="$FLAGS -L$PREFIX/lib -Wl,-headerpad_max_install_names")
    fi
    # ninja does not see the libraries change (a rebuild by
    # build-dist-deps.sh): relink when one is newer than the binaries
    for b in $BINARIES; do
        if [ -f "$B/$b" ] && [ -n "$(find "$PREFIX/lib" -name '*.dylib' -newer "$B/$b" | head -1)" ]; then
            rm -f "$B/$b" "$B/$b-unsigned"
        fi
    done
    # shellcheck disable=SC2086
    ninja -C "$B" $BINARIES
done

# the universal binaries, and what bundle-emulator.sh takes beside them
rm -rf "$OUT"
mkdir -p "$OUT"
for b in $BINARIES; do
    slices=()
    for ARCH in $ARCHS; do slices+=("$D/qemu-build-$ARCH/$b"); done
    lipo -create "${slices[@]}" -output "$OUT/$b"
done
first=$(echo $ARCHS | awk '{print $1}')
mkdir -p "$OUT/qemu-bundle/usr/local/share"
cp -RL "$D/qemu-build-$first/qemu-bundle/usr/local/share/icons" \
    "$OUT/qemu-bundle/usr/local/share/icons"

for b in $BINARIES; do
    printf "%-18s %-14s macOS %s, %s libraries from dist/prefix\n" "$b" \
        "$(lipo -archs "$OUT/$b")" \
        "$(vtool -show-build "$OUT/$b" | awk '/minos/{print $2; exit}')" \
        "$(otool -L "$OUT/$b" | grep -c "$D/prefix/lib")"
done
