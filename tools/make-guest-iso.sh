#!/bin/sh
# Package the guest driver sources as a CD image the guest can mount.
# Usage: tools/make-guest-iso.sh [output.iso]   (default vms/guest-src.iso)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/vms/guest-src.iso"}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

cp -R "$root/guest/." "$stage/"
find "$stage" -name build -type d -prune -exec rm -rf {} +
# The guest/host protocol header lives in the QEMU tree; ship a copy.
inc="$root/qemu/include/hw/display"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-image.h" "$stage/PPCOSXFramebuffer/"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-ops.h" "$inc/ppcosx-gl-image.h" "$stage/libppcosxgl/"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-ops.h" "$inc/ppcosx-gl-image.h" "$stage/PPCOSXGL/"

mkdir -p "$(dirname "$out")"
rm -f "$out"
hdiutil makehybrid -quiet -iso -joliet -hfs -default-volume-name PPCOSXSRC \
    -o "$out" "$stage"
echo "$out"
