#!/bin/sh
# Copy the emulator into the ppcOSX app so it runs on its own:
#
#   tools/bundle-emulator.sh path/to/ppcOSX.app
#
#   Contents/Resources/qemu/bin/      qemu-system-ppc, qemu-system-ppc64,
#                                     qemu-img (from build/, or the
#                                     QEMU_BUILD folder; Release builds use
#                                     dist/qemu-build, see build-dist-qemu.sh)
#   Contents/Resources/qemu/share/    firmware the Mac machines use; QEMU is
#                                     built relocatable and looks in
#                                     ../share/qemu beside its binary
#   Contents/Resources/scripts/       the Open Firmware Forth generators
#   Contents/Frameworks/              every non-system library they link
#                                     (Homebrew's glib, pixman...), relinked
#                                     to load from the bundle
#
# Each copied binary and library is signed again (ad hoc): changing a
# Mach-O's load commands invalidates its signature.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$1
[ -d "$APP/Contents" ] || { echo "usage: $0 path/to/ppcOSX.app" >&2; exit 1; }
if [ -z "$QEMU_BUILD" ] && [ "$CONFIGURATION" = Release ]; then
    QEMU_BUILD="$ROOT/dist/qemu-build"
fi
BUILD="${QEMU_BUILD:-$ROOT/build}"
PCBIOS="$ROOT/qemu/pc-bios"
RES="$APP/Contents/Resources"
QEMU="$RES/qemu"
FW="$APP/Contents/Frameworks"

for b in qemu-system-ppc qemu-system-ppc64 qemu-img; do
    [ -x "$BUILD/$b" ] || { echo "$BUILD/$b missing: build the fork first" >&2; exit 1; }
done

rm -rf "$QEMU" "$RES/scripts" "$FW"
mkdir -p "$QEMU/bin" "$QEMU/share/qemu" "$RES/scripts" "$FW"
for b in qemu-system-ppc qemu-system-ppc64 qemu-img; do
    cp "$BUILD/$b" "$QEMU/bin/$b"
    chmod u+w "$QEMU/bin/$b"
done

# firmware: OpenBIOS, the VGA ndrv and BIOSes, keymaps
for f in openbios-ppc qemu_vga.ndrv vgabios-stdvga.bin vgabios-ati.bin; do
    cp -L "$PCBIOS/$f" "$QEMU/share/qemu/$f"
done
cp -RL "$PCBIOS/keymaps" "$QEMU/share/qemu/keymaps" 2>/dev/null || true
rm -f "$QEMU/share/qemu/keymaps/meson.build"
if [ -d "$BUILD/qemu-bundle/usr/local/share/icons" ]; then
    cp -RL "$BUILD/qemu-bundle/usr/local/share/icons" "$QEMU/share/icons"
fi

cp "$ROOT/tools/ati-agp-nvramrc.sh" "$ROOT/tools/smp-nvramrc.sh" "$ROOT/tools/sound-nvramrc.sh" "$RES/scripts/"
# CPU profiles and card settings, from ppcosx itself
python3 "$ROOT/ppcosx" machine-data > "$RES/scripts/machine-data.json"
chmod +x "$RES/scripts/"*.sh

# libraries outside the system: copy, following each one's own links
is_foreign() {
    case "$1" in
        /System/*|/usr/lib/*|@*) return 1 ;;
        *) return 0 ;;
    esac
}

# the libraries a binary links, in every architecture of a universal one
# (otool heads each with a "FILE (architecture ARCH):" line)
deps() {
    otool -L "$1" | grep -v ':$' | awk '{print $1}' | sort -u
}

queue=$(mktemp)
done_list=$(mktemp)
trap 'rm -f "$queue" "$done_list"' EXIT
for b in "$QEMU/bin/"*; do
    deps "$b" >> "$queue"
done
while [ -s "$queue" ]; do
    lib=$(head -1 "$queue")
    sed -i '' 1d "$queue"
    is_foreign "$lib" || continue
    name=$(basename "$lib")
    grep -qx "$name" "$done_list" && continue
    echo "$name" >> "$done_list"
    real=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$lib" 2>/dev/null \
           || readlink -f "$lib")
    cp "$real" "$FW/$name"
    chmod u+w "$FW/$name"
    deps "$FW/$name" >> "$queue"
done

# relink: binaries load from ../../../Frameworks, libraries from beside them
relink() {
    file=$1; prefix=$2
    for lib in $(deps "$file"); do
        is_foreign "$lib" || continue
        install_name_tool -change "$lib" "$prefix/$(basename "$lib")" "$file" 2>/dev/null
    done
}
for b in "$QEMU/bin/"*; do
    relink "$b" "@executable_path/../../../Frameworks"
done
for l in "$FW/"*.dylib; do
    [ -f "$l" ] || continue
    install_name_tool -id "@loader_path/$(basename "$l")" "$l" 2>/dev/null
    relink "$l" "@loader_path"
done

for l in "$FW/"*.dylib "$QEMU/bin/"*; do
    [ -f "$l" ] || continue
    codesign --force --sign - "$l" 2>/dev/null
done

echo "bundled the emulator into $APP: $(ls "$FW" | wc -l | tr -d ' ') libraries, $(du -sh "$QEMU" | cut -f1) in Resources/qemu"
