#!/bin/sh
# Run one of the guest GL tests (guest/tests/NAME.c) on Apple's own GL
# driver in a guest started by tools/ati-probe.sh with ATI_KEEP=1, and
# screenshot it.
#
#   tools/ati-gltest.sh LABEL NAME [SECONDS]
#
# The screenshot goes to vms/ati-probe/LABEL/NAME.png.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
label=$1
name=$2
secs=${3:-8}
out="vms/ati-probe/$label"
mkdir -p "$out"
tools/guest-ssh.sh "killall $name >/dev/null 2>&1; cd ~/src/tests &&
    gcc -o /tmp/$name $name.c -framework GLUT -framework OpenGL 2>&1 | tail -3"
# The app keeps its SSH session open, so that session runs in the
# background here and is ended once the screenshot is taken.
tools/guest-ssh.sh "cd /tmp && DYLD_INSERT_LIBRARIES= /tmp/$name" \
    > "$out/$name.log" 2>&1 &
ssh_pid=$!
sleep "$secs"
python3 tools/guestctl.py shot "$root/$out/$name.png" >/dev/null 2>&1
tools/guest-ssh.sh "killall $name >/dev/null 2>&1"
sleep 1
kill "$ssh_pid" 2>/dev/null
tail -5 "$out/$name.log"
echo "$out/$name.png"
