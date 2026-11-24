#!/bin/bash
# Build the libraries QEMU uses, for the distributable ppcOSX app: universal
# (arm64 and x86_64) dylibs that run on macOS 14 and later. Homebrew's
# copies are built for the Mac they were installed on (macOS 26+ here, one
# architecture), so the app cannot use them.
#
#   tools/build-dist-deps.sh            download what is missing, build all
#   tools/build-dist-deps.sh NAME...    rebuild only these
#   ARCHS=x86_64 tools/build-dist-deps.sh ...   only one architecture
#
# Each architecture is built on its own (GMP, Nettle and OpenSSL have
# assembly for each CPU) into dist/prefix-ARCH, from dist/work-ARCH/NAME;
# QEMU is built against these. Their dylibs are then merged with lipo into
# dist/prefix/lib, the universal ones the app bundles. Every dylib's install
# name is already its dist/prefix/lib path, so both halves of a binary
# linked against them name the same libraries.
#
# Sources go in dist/src/NAME/ (each archive in its own folder).
# pkg-config sees only the architecture's prefix, so nothing from Homebrew
# is picked up; libffi, zlib and curl come from macOS itself.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D="$ROOT/dist"
UNI="$D/prefix"
SRC="$D/src"
ARCHS=${ARCHS:-arm64 x86_64}
mkdir -p "$UNI/lib" "$SRC"

export MACOSX_DEPLOYMENT_TARGET=14.0
unset PKG_CONFIG_PATH CPATH LIBRARY_PATH
SDK=$(xcrun --show-sdk-path)
JOBS=$(sysctl -n hw.ncpu)

# per architecture: compiler flags, prefix, autotools host, Meson cross file
set_arch() {
    ARCH=$1
    PREFIX="$D/prefix-$ARCH"
    WORK="$D/work-$ARCH"
    mkdir -p "$PREFIX/lib/pkgconfig" "$WORK"
    ARCHFLAGS="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
    export CFLAGS="$ARCHFLAGS -O2"
    export CXXFLAGS="$ARCHFLAGS -O2"
    export CPPFLAGS="-I$PREFIX/include"
    export LDFLAGS="$ARCHFLAGS -L$PREFIX/lib -Wl,-headerpad_max_install_names"
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
    if [ "$ARCH" = arm64 ]; then
        HOST=aarch64-apple-darwin
        OPENSSL_TARGET=darwin64-arm64-cc
        CPU_FAMILY=aarch64
    else
        HOST=x86_64-apple-darwin
        OPENSSL_TARGET=darwin64-x86_64-cc
        CPU_FAMILY=x86_64
    fi
    # Meson takes the machine from a cross file (pixman picks its SIMD code
    # by it); Rosetta runs what the build runs for x86_64
    CROSS="$WORK/meson-cross.ini"
    cat > "$CROSS" <<EOF
[binaries]
c = ['cc', '-arch', '$ARCH']
cpp = ['c++', '-arch', '$ARCH']
objc = ['cc', '-arch', '$ARCH']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = false

[host_machine]
system = 'darwin'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU_FAMILY'
endian = 'little'
EOF
    write_system_pcs
}

# libffi, zlib, curl: macOS's own (universal), described for pkg-config
write_system_pcs() {
    cat > "$PREFIX/lib/pkgconfig/libffi.pc" <<EOF
Name: libffi
Description: libffi from the macOS SDK
Version: 3.4.0
Cflags: -I$SDK/usr/include/ffi
Libs: -lffi
EOF
    cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: zlib from macOS
Version: $(sed -n 's/#define ZLIB_VERSION "\(.*\)"/\1/p' "$SDK/usr/include/zlib.h")
Cflags:
Libs: -lz
EOF
    cat > "$PREFIX/lib/pkgconfig/libcurl.pc" <<EOF
Name: libcurl
Description: libcurl from macOS
Version: $(sed -n 's/#define LIBCURL_VERSION "\(.*\)"/\1/p' "$SDK/usr/include/curl/curlver.h")
Cflags:
Libs: -lcurl
EOF
}

# build tools: meson from QEMU's own venv, CMake in a venv of our own
if [ ! -x "$D/venv/bin/cmake" ]; then
    python3 -m venv "$D/venv"
    "$D/venv/bin/pip" install --quiet cmake
fi
MESON="$ROOT/build/pyvenv/bin/meson"
export PATH="$D/venv/bin:$PATH"

# name -> url
URLS=(
  "zstd https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
  "libpng https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz"
  "libjpeg-turbo https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz"
  "libusb https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2"
  "capstone https://github.com/capstone-engine/capstone/releases/download/5.0.9/capstone-5.0.9.tar.xz"
  "gmp https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz"
  "nettle https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz"
  "libtasn1 https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.21.0.tar.gz"
  "gnutls https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.14.tar.xz"
  "openssl https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz"
  "libssh https://www.libssh.org/files/0.11/libssh-0.11.5.tar.xz"
  "glib https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz"
  "pixman https://www.cairographics.org/releases/pixman-0.46.4.tar.gz"
  "libslirp https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.7/libslirp-v4.9.7.tar.gz"
)
ORDER="zstd libpng libjpeg-turbo libusb capstone gmp nettle libtasn1 gnutls openssl libssh glib pixman libslirp"

url_of() {
    for e in "${URLS[@]}"; do
        set -- $e
        [ "$1" = "$2" ] 2>/dev/null || true
        if [ "$1" = "$NAME" ]; then echo "$2"; return; fi
    done
}

# fetch NAME: download into dist/src/NAME/, unpack, print the source dir
fetch() {
    local url file dir
    url=$(url_of)
    dir="$SRC/$NAME"
    file="$dir/$(basename "$url")"
    mkdir -p "$dir"
    if [ ! -f "$file" ]; then
        echo "== downloading $NAME" >&2
        curl -fL --retry 3 -o "$file.part" "$url" && mv "$file.part" "$file"
    fi
    if [ ! -d "$dir/unpacked" ]; then
        mkdir -p "$dir/unpacked"
        tar -xf "$file" -C "$dir/unpacked"
    fi
    echo "$dir/unpacked/$(ls "$dir/unpacked" | head -1)"
}

cmake_build() {   # cmake_build SRCDIR ARGS...
    local s=$1; shift
    rm -rf "$WORK/$NAME"
    cmake -S "$s" -B "$WORK/$NAME" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_SYSTEM_PROCESSOR=$CPU_FAMILY \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" "$@"
    cmake --build "$WORK/$NAME" -j "$JOBS"
    cmake --install "$WORK/$NAME"
}

autotools_build() {   # autotools_build SRCDIR ARGS...
    local s=$1; shift
    rm -rf "$WORK/$NAME"; mkdir -p "$WORK/$NAME"
    (cd "$WORK/$NAME" && "$s/configure" --prefix="$PREFIX" --enable-shared \
        --disable-static --host=$HOST --build=aarch64-apple-darwin "$@" \
        && make -j "$JOBS" && make install)
}

meson_build() {   # meson_build SRCDIR ARGS...
    local s=$1; shift
    rm -rf "$WORK/$NAME"
    "$MESON" setup "$WORK/$NAME" "$s" --prefix="$PREFIX" --libdir=lib \
        --buildtype=release -Ddefault_library=shared --cross-file "$CROSS" "$@"
    ninja -C "$WORK/$NAME" install
}

build() {
    local s
    s=$(fetch)
    echo "== building $NAME ($s)"
    case $NAME in
    zstd)
        # built in its source tree: clean out the other architecture's
        make -C "$s/lib" clean >/dev/null
        make -C "$s/lib" -j "$JOBS" PREFIX="$PREFIX" install-shared install-includes install-pc
        ;;
    libpng)
        cmake_build "$s" -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
            -DPNG_FRAMEWORK=OFF
        ;;
    libjpeg-turbo)
        cmake_build "$s" -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=OFF
        ;;
    libusb)
        autotools_build "$s"
        ;;
    capstone)
        cmake_build "$s" -DBUILD_SHARED_LIBS=ON -DCAPSTONE_BUILD_TESTS=OFF \
            -DCAPSTONE_BUILD_CSTOOL=OFF
        # its CMakeLists builds universal on macOS whatever the setting
        for l in "$PREFIX"/lib/libcapstone*.dylib; do
            [ -L "$l" ] || lipo "$l" -thin "$ARCH" -output "$l"
        done
        ;;
    gmp)
        autotools_build "$s"
        ;;
    nettle)
        autotools_build "$s" --disable-documentation --disable-openssl
        ;;
    libtasn1)
        autotools_build "$s" --disable-doc
        ;;
    gnutls)
        autotools_build "$s" --with-included-unistring --without-p11-kit \
            --without-idn --without-tpm --without-tpm2 --without-brotli \
            --without-zstd --disable-doc --disable-tests --disable-tools \
            --disable-cxx --disable-guile --disable-nls
        ;;
    openssl)
        rm -rf "$WORK/$NAME"; mkdir -p "$WORK/$NAME"
        (cd "$WORK/$NAME" && "$s/Configure" "$OPENSSL_TARGET" shared no-tests \
            no-docs --prefix="$PREFIX" --libdir=lib \
            -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET &&
         make -j "$JOBS" && make install_sw)
        ;;
    libssh)
        cmake_build "$s" -DWITH_GSSAPI=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF \
            -DWITH_ZLIB=ON -DOPENSSL_ROOT_DIR="$PREFIX"
        ;;
    glib)
        # libffi from the SDK (macOS has it, write_system_pcs), so meson
        # needs no download
        meson_build "$s" -Dtests=false -Dnls=disabled -Dintrospection=disabled \
            -Dman-pages=disabled -Ddocumentation=false -Dlibmount=disabled \
            -Dselinux=disabled -Dlibelf=disabled -Dsysprof=disabled \
            -Ddtrace=disabled -Dsystemtap=disabled --force-fallback-for=pcre2
        ;;
    pixman)
        meson_build "$s" -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
            -Dlibpng=disabled
        ;;
    libslirp)
        meson_build "$s"
        ;;
    esac
}

# Point every dylib of this architecture's prefix (its own name and the
# libraries it links) at dist/prefix/lib, where the universal ones go, and
# sign it again (ad hoc) after the change.
fix_install_names() {
    local l dep n
    for l in "$PREFIX/lib"/*.dylib; do
        [ -L "$l" ] && continue
        # its own install name's file name (libnettle.8.dylib, not the file
        # libnettle.8.11.dylib), which is what other libraries link: a
        # second name would load the library twice
        n=$(basename "$(otool -D "$l" | sed -n 2p)")
        install_name_tool -id "$UNI/lib/$n" "$l" 2>/dev/null
        for dep in $(otool -L "$l" | sed 1d | awk '{print $1}'); do
            case $dep in
            "$PREFIX/lib/"*)
                install_name_tool -change "$dep" "$UNI/lib/${dep##*/}" "$l" 2>/dev/null ;;
            esac
        done
        codesign --force --sign - "$l" 2>/dev/null
    done
}

# Merge the architectures' dylibs into universal ones in dist/prefix/lib
# (symbolic links copied as they are).
merge() {
    local first l n others a
    first=$(echo $ARCHS | awk '{print $1}')
    rm -f "$UNI/lib"/*.dylib
    for l in "$D/prefix-$first/lib"/*.dylib; do
        n=$(basename "$l")
        if [ -L "$l" ]; then
            cp -P "$l" "$UNI/lib/$n"
            continue
        fi
        others=()
        for a in $ARCHS; do others+=("$D/prefix-$a/lib/$n"); done
        lipo -create "${others[@]}" -output "$UNI/lib/$n"
    done
}

if [ $# -gt 0 ]; then ORDER="$*"; fi
for A in $ARCHS; do
    set_arch "$A"
    echo "==== $ARCH"
    for NAME in $ORDER; do
        build
    done
    fix_install_names
done
merge
echo "== done: $(ls "$UNI/lib"/*.dylib | wc -l | tr -d ' ') dylibs in $UNI/lib ($ARCHS)"
