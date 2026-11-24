#!/bin/sh
# Print the Open Firmware nvramrc that describes the UniNorth AGP bus and an
# emulated Radeon 7000 in slot 16 on it.
#
# QEMU's mac99 assigns the AGP card's BARs at reset (core99_agp_firmware):
# BAR0 (both apertures) 0x90000000, BAR1 (I/O) 0x400, BAR2 0x94000000,
# interrupt pin A -> OpenPIC 0x1b. OpenBIOS does not scan the AGP bus, so
# build its node and the card's here, as a PowerMac3,x's ROM would.
# Pair it with: -device ati-vga,model=rv100,romfile=,agp=on,bus=agp,addr=0x10
#
# Environment, for other cards:
#   ATI_NAME   node name (default ATY,Rage6ag)
#   ATI_MODEL  model property (default ATY,RV100ad)
#   ATI_DEVID  PCI device id, hex (default 5159)
#   ATI_BAR0   BAR0 size, hex: twice the VRAM (default 4000000). BAR2 (the
#              registers) follows it, as core99_agp_firmware assigns BARs
#              largest first from 0x90000000.
#   ATI_BAR2   BAR2 (register) size, hex (default 4000; 10000 on R300)
#   ATI_FORTH  extra Forth run inside the card's node
#   ATI_AFTER_FORTH  Forth run once the nodes are complete, e.g. to make
#              the card the console
#   ATI_BUS_FORTH  extra Forth run inside the AGP bus node (before the
#              card's), e.g. open and close methods so that Open Firmware
#              clients can open the card
#   ATI_G5     if set, for a G5 (PowerPC 970): QEMU's G5 has a single bus,
#              the U3's AGP bus, which OpenBIOS scans itself, so only name
#              the card's node (/pci@f0000000/pci1002,DEVID@10) and give it
#              its model.
name=${ATI_NAME:-ATY,Rage6ag}
model=${ATI_MODEL:-ATY,RV100ad}
devid=${ATI_DEVID:-5159}
bar0=${ATI_BAR0:-4000000}
bar2sz=${ATI_BAR2:-4000}
bar2=$(printf %x $((0x90000000 + 0x$bar0)))
if [ -n "${ATI_G5:-}" ]; then
    # OpenBIOS declares only 0x80000000-0x8fffffff for the bus but places
    # BARs above it (the card's registers at 0x90000000), and Mac OS X
    # gives up on a bus with devices outside its ranges. QEMU decodes
    # 0x80000000-0xefffffff: make the memory range (the last entry; its
    # size is the last cell) say so.
    n="\" /pci@f0000000\" find-device"
    n="$n \" ranges\" active-package get-package-property 0= if"
    n="$n + 4 - 70000000 swap l! then"
    n="$n \" /pci@f0000000/pci1002,$devid@10\" find-device"
    n="$n \" $name\" device-name \" display\" device-type"
    n="$n \" $model\" encode-string \" model\" property"
    [ -n "${ATI_FORTH:-}" ] && n="$n $ATI_FORTH"
    echo "$n device-end"
    exit 0
fi
# Short words keep the whole nvramrc (with tools/smp-nvramrc.sh's) within
# the space NVRAM has for it.
e="ei"
p="ep"
n="device-end : ei encode-int ; : ep ei encode+ ;"
n="$n \" /\" find-device new-device \" pci\" device-name \" pci\" device-type"
n="$n \" AAPL,UniNorth\" encode-string \" model\" property"
n="$n \" uni-north\" encode-string \" compatible\" property"
n="$n 3 $e \" #address-cells\" property 2 $e \" #size-cells\" property"
n="$n 1 $e \" #interrupt-cells\" property"
n="$n 3f940aa $e \" clock-frequency\" property"
n="$n f0000000 $e 2000000 $p \" reg\" property"
n="$n 0 $e 0 $p \" bus-range\" property"
n="$n 1000000 $e 0 $p 0 $p f0000000 $p 0 $p 800000 $p"
n="$n 2000000 $p 0 $p 90000000 $p 90000000 $p 0 $p 10000000 $p \" ranges\" property"
n="$n \" /pci@f2000000/mac-io@c/interrupt-controller\" find-package drop"
n="$n dup $e \" interrupt-parent\" property"
n="$n 8000 $e 0 $p 0 $p 1 $p rot $p 1b $p 1 $p \" interrupt-map\" property"
n="$n fff800 $e 0 $p 0 $p 7 $p \" interrupt-map-mask\" property"
[ -n "${ATI_BUS_FORTH:-}" ] && n="$n $ATI_BUS_FORTH"
n="$n new-device \" $name\" device-name \" display\" device-type"
# WindowServer reads every IOAGPDevice's model (and rejects ATY,Rage128);
# without one it crashes and Quartz Extreme stays off.
n="$n \" $model\" encode-string \" model\" property"
n="$n 1002 $e \" vendor-id\" property $devid $e \" device-id\" property"
n="$n 0 $e \" revision-id\" property 30000 $e \" class-code\" property"
n="$n 1 $e \" interrupts\" property"
n="$n 8000 $e 0 $p 0 $p 0 $p 0 $p"
n="$n 42008010 $p 0 $p 0 $p 0 $p $bar0 $p"
n="$n 1008014 $p 0 $p 0 $p 0 $p 100 $p"
n="$n 2008018 $p 0 $p 0 $p 0 $p $bar2sz $p \" reg\" property"
n="$n c2008010 $e 0 $p 90000000 $p 0 $p $bar0 $p"
n="$n 81008014 $p 0 $p 400 $p 0 $p 100 $p"
n="$n 82008018 $p 0 $p $bar2 $p 0 $p $bar2sz $p \" assigned-addresses\" property"
[ -n "${ATI_FORTH:-}" ] && n="$n $ATI_FORTH"
n="$n finish-device finish-device device-end"
[ -n "${ATI_AFTER_FORTH:-}" ] && n="$n $ATI_AFTER_FORTH"
echo "$n"
