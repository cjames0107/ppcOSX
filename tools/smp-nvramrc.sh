#!/bin/sh
# Print Open Firmware Forth (for nvramrc) that describes CPUs 1..N-1.
#
#   tools/smp-nvramrc.sh N
#
# OpenBIOS puts only the boot CPU under /cpus. Mac OS X counts /cpus's
# children to decide how many CPUs to start (AppleCore99PE), so add a node
# per secondary CPU: a copy of CPU 0's properties with its own "reg" and
# "state" "stopped" (Core99CPU requires a state; "running" marks the boot
# CPU) and "soft-reset", the KeyLargo GPIO that starts it (0x5c, 0x67,
# 0x68 for CPUs 1-3, as on Apple's multiprocessor Core99 machines): Apple's
# MacRISC2CPU strobes the line that property names, and without it never
# started CPUs 2 and 3, though the interrupt controller still sent them
# device interrupts. QEMU's mac99 starts each CPU when its line is strobed.
# Prints nothing for N <= 1.
#
# Everything runs with /cpus selected, from the first definition to the
# final device-end: in OpenBIOS, find-device and device-end change the
# search order, so words defined in one context aren't found in another.
# (cpu-add used to select /cpus and end with device-end; after the first
# call the interpreter could no longer find cpu-add, "cpu-add:" and
# OpenBIOS stopped, so only two CPUs ever worked.)
n=${1:-1}
[ "$n" -gt 1 ] 2>/dev/null || exit 0
f="\" /cpus\" find-device"
f="$f 0 value cpu0 \" /cpus\" find-package drop child to cpu0"
f="$f : cpu-skip? 2dup \" reg\" strcmp 0= >r \" state\" strcmp 0= r> or ;"
f="$f : cpu-copy 0 0 begin cpu0 next-property while"
f="$f 2dup cpu0 get-package-property 0= if 2over cpu-skip? if 2drop"
f="$f else encode-bytes 2over property then then repeat ;"
f="$f : cpu-add ( soft-reset n -- ) new-device cpu-copy"
f="$f encode-int \" reg\" property encode-int \" soft-reset\" property"
f="$f \" stopped\" encode-string \" state\" property"
f="$f finish-device ;"
i=1
while [ "$i" -lt "$n" ]; do
    case $i in 1) r=5c ;; 2) r=67 ;; *) r=68 ;; esac
    f="$f $r $i cpu-add"
    i=$((i + 1))
done
echo "$f device-end"
