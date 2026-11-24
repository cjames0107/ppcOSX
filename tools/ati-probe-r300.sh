#!/bin/sh
# tools/ati-probe.sh with an emulated Radeon 9600 (RV350, 64MB) on the AGP
# bus, named as the drivers expect. Same arguments (LABEL; NAME is ignored)
# and environment as tools/ati-probe.sh.
ATI_AGP=1
ATI_NAME=${ATI_NAME:-ATY,Pheonix}
ATI_MODEL=${ATI_MODEL:-ATY,RV350AP}
ATI_DEVID=4150
ATI_BAR0=8000000
ATI_BAR2=10000
ATI_DEVICE=${ATI_DEVICE:-ati-vga,model=rv350,romfile=,vgamem_mb=64}
export ATI_AGP ATI_NAME ATI_MODEL ATI_DEVID ATI_BAR0 ATI_BAR2 ATI_DEVICE
exec "$(dirname "$0")/ati-probe.sh" "$1" "$ATI_NAME"
