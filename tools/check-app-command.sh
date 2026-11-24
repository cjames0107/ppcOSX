#!/bin/bash
# Check that the ppcOSX app and `ppcosx run --vm` build the same QEMU
# command line for a set of VM configurations (the app mirrors ppcosx's
# build_command in Swift).
#
#   tools/check-app-command.sh [path/to/ppcOSX.app]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=${1:-$ROOT/app/build/Debug/ppcOSX.app}
BIN="$APP/Contents/MacOS/ppcOSX"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
touch "$T/disk.qcow2" "$T/second.img" "$T/usb.img" "$T/install.iso"

n=0; fail=0
check() {   # check NAME JSON-FIELDS
    n=$((n + 1))
    d="$T/vm$n"; mkdir -p "$d"
    cat > "$d/config.json" <<JSON
{"name":"$1","drives":[{"id":"disk0","type":"disk","file":"$T/disk.qcow2"},
 {"id":"cdrom0","type":"cdrom","file":"$T/install.iso"}],$2}
JSON
    python3 "$ROOT/ppcosx" run --vm "$d" --qmp-socket "$d/qmp.sock" --display-socket "$d/display.sock" --dry-run-json 2>/dev/null |
        python3 -c 'import json,sys,os; c=json.load(sys.stdin); c[0]=os.path.basename(c[0]); print(json.dumps(c))' > "$d/py.json"
    "$BIN" --print-command "$d" > "$d/app.json"
    if python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1])) != json.load(open(sys.argv[2])))' "$d/py.json" "$d/app.json"; then
        echo "same:      $1"
    else
        echo "DIFFERENT: $1"
        python3 - "$d/py.json" "$d/app.json" <<'PY'
import json, sys, difflib
a, b = (json.load(open(f)) for f in sys.argv[1:3])
for l in difflib.unified_diff(a, b, "ppcosx", "app", lineterm="", n=1):
    print("   ", l[:160])
PY
        fail=1
    fi
}

check "Tiger G4" '"guest":"10.4","cpu":"g4","cores":1,"memory":1024,"graphics":"radeon9600"'
check "Leopard dual G5" '"guest":"10.5","cpu":"g5","cores":2,"memory":2048,"graphics":"radeon9600","boot":"cdrom"'
check "Panther G3 7000" '"guest":"10.3","cpu":"g3","memory":512,"graphics":"radeon7000","resolution":"800x600x32"'
check "Jaguar quad G4" '"guest":"10.2","cpu":"g4","cores":4,"graphics":"radeon7000","network":{"mode":"nat","forwards":[{"protocol":"tcp","host":2222,"guest":22},{"protocol":"udp","host":5353,"guest":53}]}'
check "Cheetah Rage 128" '"guest":"10.0","cpu":"g4","memory":512,"graphics":"rage128","input":"auto"'
check "Puma VGA no network" '"guest":"10.1","cpu":"g3","graphics":"vga","network":{"mode":"none"}'
check "OS 9 ADB" '"guest":"9.2","cpu":"g4","memory":512,"graphics":"rage128"'
check "Tiger 7447A USB input" '"guest":"10.4","cpu":"g4-7447a","input":"usb","graphics":"radeon9600"'
check "Tiger sound off" '"version":2,"guest":"10.4","audio":{"enabled":false}'
check "Panther sound on" '"version":2,"guest":"10.3","audio":{"enabled":true}'
check "Leopard screen sharing" '"guest":"10.5","sharing":{"enabled":true,"port":5901,"remote":false,"tls":false,"jpeg":false}'
check "Tiger remote TLS JPEG" '"guest":"10.4","sharing":{"enabled":true,"port":5905,"remote":true,"tls":true,"jpeg":true}'
check "Panther USB passthrough" '"guest":"10.3","usb":[{"vendor":1452,"product":4386,"name":"Keyboard"},{"vendor":2316,"product":4096,"name":"Stick"}]'
n=$((n + 1)); d="$T/vm$n"; mkdir -p "$d"
cat > "$d/config.json" <<JSON
{"name":"Drives","guest":"10.4","drives":[{"id":"disk0","type":"disk","file":"$T/disk.qcow2"},
 {"id":"disk1","type":"disk","file":"$T/second.img","readOnly":true},{"id":"usb0","type":"usb","file":"$T/usb.img"},
 {"id":"cdrom0","type":"cdrom","file":""},{"id":"cdrom1","type":"cdrom","file":"$T/missing.iso"}]}
JSON
python3 "$ROOT/ppcosx" run --vm "$d" --qmp-socket "$d/qmp.sock" --display-socket "$d/display.sock" --dry-run-json 2>/dev/null |
    python3 -c 'import json,sys,os; c=json.load(sys.stdin); c[0]=os.path.basename(c[0]); print(json.dumps(c))' > "$d/py.json"
"$BIN" --print-command "$d" > "$d/app.json"
if cmp -s <(python3 -m json.tool "$d/py.json") <(python3 -m json.tool "$d/app.json") 2>/dev/null ||
   python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1])) != json.load(open(sys.argv[2])))' "$d/py.json" "$d/app.json"; then
    echo "same:      drives (two disks, USB, empty and missing discs)"
else
    echo "DIFFERENT: drives"; fail=1
fi
n=$((n + 1)); d="$T/vm$n"; mkdir -p "$d"
cat > "$d/config.json" <<JSON
{"name":"Remote","guest":"10.4","drives":[{"id":"disk0","type":"disk","file":"ssh://me@server.local/vms/tiger.qcow2"},
 {"id":"cdrom0","type":"cdrom","file":"https://example.com/discs/tiger.iso"}]}
JSON
python3 "$ROOT/ppcosx" run --vm "$d" --qmp-socket "$d/qmp.sock" --display-socket "$d/display.sock" --dry-run-json 2>/dev/null |
    python3 -c 'import json,sys,os; c=json.load(sys.stdin); c[0]=os.path.basename(c[0]); print(json.dumps(c))' > "$d/py.json"
"$BIN" --print-command "$d" > "$d/app.json"
if python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1])) != json.load(open(sys.argv[2])))' "$d/py.json" "$d/app.json"; then
    echo "same:      remote drives (ssh:// disk, https:// disc)"
else
    echo "DIFFERENT: remote drives"; diff "$d/py.json" "$d/app.json" | head; fail=1
fi
exit $fail
