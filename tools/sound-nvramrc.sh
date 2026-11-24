#!/bin/sh
# Print the Open Firmware Forth that describes the Screamer sound chip
# QEMU's mac99 has when given an audiodev (hw/audio/screamer.c), as a
# Power Mac's ROM would: /pci@f2000000/mac-io@c/davbus (on a G5,
# /pci@f0000000/mac-io@c/davbus), with a "sound"
# child that Mac OS (AppleScreamerAudio, the Sound Manager) reads.
#
#   davbus  reg: the codec at mac-io 0x14000, output DBDMA at 0x8800,
#           input DBDMA at 0x8900; interrupts: codec 0x18 (level), output
#           DMA 0x09, input DMA 0x0a (edge), as QEMU wires them, on the
#           mac-io's interrupt controller (named, as Mac OS X's kernel
#           stops early at a mac-io child without interrupt-parent)
#   sound   model 343S0184 (Screamer), and "sound-objects": NUL-separated
#           strings, one per port, of "key value" pairs that Apple's
#           AudioDeviceTreeParser reads (numbers in decimal or 0x hex):
#           one output, the internal speaker ('ispk'), always present,
#           driven as the codec's output 2.
#
# It is not part of nvramrc itself but the value of its own variable,
# ppcosx-sound, which nvramrc evaluates (SOUND_LOADER below, in ppcosx and
# the app): Mac OS X 10.0 does not start (BootX returns to Open Firmware)
# when nvramrc is longer than about 2520 bytes, and the Rage 128's Forth
# alone is 2250. Short words still keep it small; the counts of inputs,
# detects and features may be absent (Apple's parser logs it and takes 0),
# but not an output's fields: without them the driver mutes every output.
#
#   -prom-env "ppcosx-sound=$(tools/sound-nvramrc.sh)" and, in nvramrc:
#   " ppcosx-sound" " /options" find-package drop get-package-property
#   0= if evaluate then
# SOUND_G5=1: QEMU's G5 has a single PCI bus, with the mac-io on it
macio=/pci@f2000000/mac-io@c
[ -n "${SOUND_G5:-}" ] && macio=/pci@f0000000/mac-io@c
n="device-end : ei encode-int ; : ep ei encode+ ; : es encode-string ;"
n="$n : p property ;"
n="$n \" $macio\" find-device new-device \" davbus\" device-name"
n="$n 14000 ei 1000 ep 8800 ep 100 ep 8900 ep 100 ep \" reg\" p"
n="$n 18 ei 1 ep 9 ep 0 ep a ep 0 ep \" interrupts\" p"
n="$n \" $macio/interrupt-controller\" find-package drop"
n="$n ei \" interrupt-parent\" p"
n="$n new-device \" sound\" device-name \" 343S0184\" es \" model\" p"
n="$n 0 ei \" device-id\" p 1 ei \" #-outputs\" p"
n="$n \" output device-mask 0 device-match 0 icon-id 0x6973706b port-connection 1"
n="$n port-type 0x6973706b name-id 0 param 2 model OutputPort\""
n="$n es \" sound-objects\" p finish-device finish-device device-end"
echo "$n"
