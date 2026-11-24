#!/bin/sh
# Tiger on a G4 with an emulated ATI Radeon on the AGP bus, in a window.
# Apple's own ATI drivers run the card, so nothing of ours is installed in
# the guest.
#
#   PPCOSX_CARD=r300  Radeon 9600 (RV350, 64MB): Quartz Extreme and Core
#                     Image (the default)
#   PPCOSX_CARD=r100  Radeon 7000 (RV100, 32MB): Quartz Extreme only
#
#   PPCOSX_CPU=g4     the CPU: g3 (PowerPC 750), g4 (7400, the default),
#                     g5 (970), or any QEMU -cpu name
#   PPCOSX_SMP=N      number of CPUs, 1-4 (a dual G4, a quad G5, ...);
#                     each gets its own host thread
#
# The disk is vms/tiger-accel.qcow2, your own copy of the dev guest made on
# first run, so changes persist and never touch vms/tiger.qcow2.
# PPCOSX_DISK picks another image. SSH: tools/guest-ssh.sh (port 2222).
# Extra arguments are passed to QEMU.
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
disk=${PPCOSX_DISK:-vms/tiger-accel.qcow2}
if [ ! -e "$disk" ]; then
    echo "run-tiger-accel: copying vms/tiger.qcow2 to $disk" >&2
    cp -c vms/tiger.qcow2 "$disk" 2>/dev/null || cp vms/tiger.qcow2 "$disk" || exit 1
fi
case "${PPCOSX_CPU:-g4}" in
g3) cpu=750_v3.1; cpuname=G3 ;;
g4) cpu=7400_v2.9; cpuname=G4 ;;
g5) cpu=970_v2.2; cpuname=G5 ;;
*) cpu=$PPCOSX_CPU; cpuname=$PPCOSX_CPU ;;
esac
smp=${PPCOSX_SMP:-1}
qemu=build/qemu-system-ppc
accel=""
case "$cpu" in 970*) qemu=build/qemu-system-ppc64; ATI_G5=1; export ATI_G5 ;; esac
if [ "$smp" -gt 1 ]; then
    # one host thread per CPU (QEMU offers that only in its 64-bit binary,
    # which runs G3s and G4s too)
    qemu=build/qemu-system-ppc64
    accel="-accel tcg,thread=multi"
    cpuname="$smp x $cpuname"
fi
case "${PPCOSX_CARD:-r300}" in
r100)
    title="Mac OS X Tiger - $cpuname - Radeon 7000 on AGP"
    card=ati-vga,model=rv100,romfile=,vgamem_mb=32
    nvramrc=$(tools/ati-agp-nvramrc.sh)
    ;;
r300)
    title="Mac OS X Tiger - $cpuname - Radeon 9600 on AGP"
    card=ati-vga,model=rv350,romfile=,vgamem_mb=64
    nvramrc=$(ATI_NAME=ATY,Pheonix ATI_MODEL=ATY,RV350AP ATI_DEVID=4150 \
              ATI_BAR0=8000000 ATI_BAR2=10000 tools/ati-agp-nvramrc.sh)
    ;;
*)
    echo "run-tiger-accel: PPCOSX_CARD is r300 or r100" >&2
    exit 1
    ;;
esac
nvramrc="$nvramrc $(tools/smp-nvramrc.sh "$smp")"
exec $qemu -name "$title" \
    -M mac99,via=pmu -cpu "$cpu" -smp "$smp" $accel -m 1024 \
    -prom-env 'auto-boot?=true' -prom-env 'use-nvramrc?=true' \
    -prom-env "nvramrc=$nvramrc" \
    -boot c \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device sungem,netdev=net0 \
    -drive "file=$disk,format=qcow2,media=disk" \
    -device usb-kbd -device usb-tablet -vga none \
    -device "$card,agp=on,bus=agp,addr=0x10,id=ati0" \
    -display cocoa,show-cursor=off \
    -monitor tcp:127.0.0.1:45455,server,nowait \
    -qmp tcp:127.0.0.1:45456,server,nowait "$@"
