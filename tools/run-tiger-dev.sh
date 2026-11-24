#!/bin/sh
# Development guest: Tiger on G4 with ppcosx-gpu, headless, QMP on 45456.
# Drive it with tools/guestctl.py. Extra arguments are passed to QEMU.
# PPCOSX_DISK picks another disk image (default vms/tiger.qcow2).
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
exec build/qemu-system-ppc -M mac99,via=pmu -cpu 7400_v2.9 -m 1024 \
    -g 1024x768x32 -prom-env 'auto-boot?=true' -prom-env 'vga-ndrv?=true' \
    -boot c \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device sungem,netdev=net0 \
    -drive "file=${PPCOSX_DISK:-vms/tiger.qcow2},format=qcow2,media=disk" \
    -drive "file=install images/10.4/MacOSX.4.iso,format=raw,media=cdrom,readonly=on" \
    -drive file=vms/guest-src.iso,format=raw,media=cdrom,readonly=on \
    -device usb-tablet -device ppcosx-gpu,id=gpu0 \
    -display none \
    -monitor tcp:127.0.0.1:45455,server,nowait \
    -qmp tcp:127.0.0.1:45456,server,nowait "$@"
