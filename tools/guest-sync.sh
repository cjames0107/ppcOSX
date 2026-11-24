#!/bin/sh
# Copy the guest sources into the running development guest (~/src) over SSH.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -R "$root/guest/." "$stage/"
find "$stage" -name build -type d -prune -exec rm -rf {} +
inc="$root/qemu/include/hw/display"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-image.h" "$stage/PPCOSXFramebuffer/"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-ops.h" "$inc/ppcosx-gl-image.h" "$stage/libppcosxgl/"
cp "$inc/ppcosx-gpu.h" "$inc/ppcosx-gl-ops.h" "$inc/ppcosx-gl-image.h" "$stage/PPCOSXGL/"
COPYFILE_DISABLE=1 tar -C "$stage" -cf - . | "$root/tools/guest-ssh.sh" 'mkdir -p ~/src && tar -C ~/src -xmf -'
