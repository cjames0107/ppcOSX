#!/bin/sh
# Measure the emulated Radeon 9600's compositing: boot a throwaway guest
# (tools/ati-probe-r300.sh), drag the Finder window around, and report how
# long the card's command processor took for the drags (2D, 3D and the
# host GPU's write-backs together).
#
#   tools/ati-dragbench.sh LABEL
#
# The environment picks the engine, e.g. PPCOSX_ATI_GPU=0 for software,
# PPCOSX_R300_THREADS=1 for software on one host thread.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
label=$1
log="vms/ati-probe/$label/qemu.log"
PPCOSX_WIDTH=1280
PPCOSX_HEIGHT=800
export PPCOSX_WIDTH PPCOSX_HEIGHT

python3 tools/guestctl.py hmp quit >/dev/null 2>&1
sleep 3
ATI_KEEP=1 ATI_TRACE=none ATI_STATS=1 tools/ati-probe-r300.sh "$label" \
    >/dev/null 2>&1
sleep 20
cp_ms() {
    grep "command processor" "$log" | tail -1 |
        sed 's/.*command processor \([0-9.]*\) ms.*/\1/'
}
before=$(cp_ms)
start=$(date +%s)
for i in 1 2 3; do
    python3 tools/guestctl.py drag 400 51 700 400 40
    sleep 2
    python3 tools/guestctl.py drag 700 400 400 51 40
    sleep 2
done
sleep 5
after=$(cp_ms)
echo "$label: command processor ${before:-0} -> ${after:-0} ms" \
     "($(( $(date +%s) - start )) s of dragging)"
python3 tools/guestctl.py shot "$root/vms/ati-probe/$label/after-drags.png"
