#!/bin/sh
# Boot a throwaway snapshot of the Tiger dev guest with an emulated ATI card
# as its only display, described in the device tree the way we choose, and
# record how Apple's ATI drivers react.
#
#   tools/ati-probe.sh LABEL NAME [FORTH]
#
#   LABEL  results go to vms/ati-probe/LABEL/
#   NAME   device-tree name for the card, e.g. ATY,Rage6ag
#   FORTH  extra Open Firmware Forth run inside the card's node, e.g.
#          '" display" device-type'
#
# Environment:
#   ATI_DEVICE   QEMU -device for the card
#                (default ati-vga,model=rv100,romfile=,vgamem_mb=32)
#   ATI_PCI_ID   its default OpenBIOS node name (default pci1002,5159)
#   ATI_WAIT     seconds to wait for SSH (default 400)
#   ATI_KEEP     if set, leave the guest running afterwards
#   ATI_AGP      if set, put the card on the UniNorth AGP bus (slot 16) and
#                describe that bus in the device tree (NAME and FORTH are
#                then ignored; the node is described by tools/ati-agp-nvramrc.sh,
#                whose ATI_NAME, ATI_MODEL, ATI_DEVID, ATI_BAR0 and ATI_FORTH
#                pick another card)
#   ATI_TRACE    QEMU trace events (default "ati_*"; "none" for none)
#   ATI_DUMP     if set, write CP packets to LABEL/packets.txt once the
#                file LABEL/packets.txt.go exists
#   ATI_BOOTARGS kernel boot-args, e.g. "-v debug=0x8" (kernel messages on
#                the serial port, in serial.log)
#   ATI_CPU      QEMU -cpu (default 7400_v2.9, a G4); 970_v2.2 etc. run
#                qemu-system-ppc64
#   ATI_SMP      number of CPUs (default 1)
#   PPCOSX_SSH_PORT, PPCOSX_QMP_PORT  host ports (default 2222, 45456; the
#                monitor is on the QMP port - 1), to run beside another guest
#   ATI_DISK     guest disk image (default vms/tiger.qcow2; booted as a
#                throwaway snapshot)
#   ATI_STATS    if set, the 3D engine prints draw statistics and texture
#                setups to LABEL/qemu.log
#
# Results: ioreg.txt (the card's IORegistry entry and children),
# dmesg.txt, trace-count.txt and trace-head.txt (register accesses),
# screen.png, serial.log.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

label=$1
name=$2
forth=${3:-}
device=${ATI_DEVICE:-ati-vga,model=rv100,romfile=,vgamem_mb=32}
pci_id=${ATI_PCI_ID:-pci1002,5159}
wait_max=${ATI_WAIT:-400}
disk=${ATI_DISK:-vms/tiger.qcow2}
out="vms/ati-probe/$label"
trace=${ATI_TRACE:-ati_*}
trace_args="-trace $trace"
[ "$trace" = none ] && trace_args=""
if [ -n "${ATI_AGP_DEBUG:-}" ]; then
    PPCOSX_AGP_DEBUG=1
    export PPCOSX_AGP_DEBUG
fi
if [ -n "${ATI_STATS:-}" ]; then
    PPCOSX_ATI_3D_STATS=1
    export PPCOSX_ATI_3D_STATS
fi
if [ -n "${ATI_DUMP:-}" ]; then
    PPCOSX_ATI_DUMP="$root/$out/packets.txt"
    export PPCOSX_ATI_DUMP
fi
# the account file beside the disk: vms/NAME-account.txt for vms/NAME.qcow2
acct=${disk%.qcow2}-account.txt
[ -f "$acct" ] || acct=vms/tiger-account.txt
pw=$(sed -n 's/^password: //p' "$acct")

cpu=${ATI_CPU:-7400_v2.9}
qemu=build/qemu-system-ppc
case "$cpu" in 970*) qemu=build/qemu-system-ppc64 ;; esac
# Several CPUs: one host thread each (multi-threaded TCG, which QEMU only
# offers in its 64-bit binary; that one runs G3s and G4s too). With one
# thread for all, a CPU spinning on a lock holds it for a whole time slice.
accel=""
if [ "${ATI_SMP:-1}" -gt 1 ]; then
    qemu=build/qemu-system-ppc64
    accel="-accel tcg,thread=multi"
fi
if pgrep -f "hostfwd=tcp:127.0.0.1:${PPCOSX_SSH_PORT:-2222}-" >/dev/null; then
    echo "ati-probe: a guest is already running; stop it first" >&2
    exit 1
fi
rm -rf "$out"
mkdir -p "$out"

nvramrc="\" /pci@f2000000/$pci_id@f\" find-device \" $name\" device-name $forth device-end"
if [ -n "${ATI_AGP:-}" ]; then
    device="$device,agp=on,bus=agp,addr=0x10"
    name=${ATI_NAME:-ATY,Rage6ag}
    case "$cpu" in 970*) ATI_G5=1; export ATI_G5 ;; esac
    nvramrc=$(tools/ati-agp-nvramrc.sh)
fi
nvramrc="$nvramrc $(tools/smp-nvramrc.sh "${ATI_SMP:-1}")"
echo "$nvramrc" > "$out/nvramrc.txt"

$qemu -M mac99,via=pmu -cpu "$cpu" -smp "${ATI_SMP:-1}" $accel -m 1024 \
    -prom-env 'auto-boot?=true' -prom-env 'use-nvramrc?=true' \
    -prom-env "nvramrc=$nvramrc" -prom-env "boot-args=${ATI_BOOTARGS:-}" \
    -boot c \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${PPCOSX_SSH_PORT:-2222}-:22 \
    -device sungem,netdev=net0 \
    -drive "file=$disk,format=qcow2,media=disk,snapshot=on" \
    -device usb-tablet -vga none -device "$device",id=ati0 \
    -display none -serial "file:$out/serial.log" \
    $trace_args -d ${ATI_QEMU_LOG:-unimp,guest_errors} ${ATI_QEMU_DFILTER:+-dfilter $ATI_QEMU_DFILTER} -D "$out/trace.log" \
    -monitor tcp:127.0.0.1:$(( ${PPCOSX_QMP_PORT:-45456} - 1 )),server,nowait \
    -qmp tcp:127.0.0.1:${PPCOSX_QMP_PORT:-45456},server,nowait > "$out/qemu.log" 2>&1 &
# Tiger's launchd stops starting sshd if it is hit too often while the
# guest boots, so leave it alone for the first two minutes.
sleep 120

start=$(date +%s)
until tools/guest-ssh.sh true 2>/dev/null; do
    if [ $(( $(date +%s) - start )) -gt "$wait_max" ]; then
        echo "no ssh after ${wait_max}s" > "$out/ioreg.txt"
        break
    fi
    sleep 20
done
sleep 10

tools/guest-ssh.sh "echo '$pw' | sudo -S pmset -a sleep 0 displaysleep 0 >/dev/null 2>&1;
    ioreg -w0 -l | awk '/-o $name@/ { on = 1 } on { print } on && /^[ |]*\\}\$/ { n++ } n >= 1 && on && /-o / && !/$name/ { exit }' | head -80;
    echo '== children:';
    ioreg -w0 -p IOService | grep -A6 -- '-o $name@' | cut -c1-150" \
    >> "$out/ioreg.txt" 2>&1
tools/guest-ssh.sh "echo '$pw' | sudo -S dmesg 2>/dev/null | tail -60" \
    > "$out/dmesg.txt" 2>&1
# Wake the display in case it went to sleep before SSH was up.
python3 tools/guestctl.py move 500 400 >/dev/null 2>&1
python3 tools/guestctl.py move 520 410 >/dev/null 2>&1
sleep 4
python3 tools/guestctl.py shot "$root/$out/screen.png" >/dev/null 2>&1

if [ -z "${ATI_KEEP:-}" ]; then
    python3 tools/guestctl.py hmp quit >/dev/null 2>&1
    sleep 3
    pkill -f "file=$disk,format=qcow2,media=disk,snapshot=on" 2>/dev/null
fi

grep -c "ati_mm_" "$out/trace.log" > "$out/trace-count.txt" 2>/dev/null
grep "ati_mm_" "$out/trace.log" | head -200 > "$out/trace-head.txt" 2>/dev/null
echo "$label: $(cat "$out/trace-count.txt") register accesses"
grep -E "class IONDRVFramebuffer|class ATIRadeon|class IODisplayConnect" "$out/ioreg.txt" | sed 's/^/  /'
