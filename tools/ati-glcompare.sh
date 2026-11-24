#!/bin/sh
# Draw the guest/tests/glcompare scenes with the accelerated renderer and
# with Apple's software renderer in a guest started by tools/ati-probe.sh
# (ATI_KEEP=1), and compare them.
#
#   tools/ati-glcompare.sh LABEL
#
# Pictures and a contact sheet (sheet.png: software, accelerated,
# difference) go to vms/ati-probe/LABEL/cmp/.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
out="vms/ati-probe/$1/cmp"
rm -rf "$out"
mkdir -p "$out"
# ATI_BINS=DIR: a prebuilt DIR/glcompare-10.4, or -10.3 for Panther (whose
# libSystem lacks cosf/sinf), for guests with no compiler
if [ -n "${ATI_BINS:-}" ]; then
    bin=glcompare-10.4
    case "$(tools/guest-ssh.sh 'sw_vers -productVersion' 2>/dev/null)" in
    10.3*) bin=glcompare-10.3 ;;
    esac
    tools/guest-ssh.sh 'cat > /tmp/glcompare && chmod +x /tmp/glcompare' \
        < "$ATI_BINS/$bin"
else
    tools/guest-ssh.sh 'cat > /tmp/glcompare.c && cd /tmp &&
        gcc -o glcompare glcompare.c -framework OpenGL 2>&1 | grep -v warning' \
        < guest/tests/glcompare.c
fi
tools/guest-ssh.sh 'cd /tmp && rm -rf hw sw; mkdir hw sw;
    ./glcompare /tmp/hw hw >/dev/null; ./glcompare /tmp/sw sw >/dev/null'
tools/guest-ssh.sh 'cd /tmp && tar cf - hw sw' | tar xf - -C "$out"
python3 tools/glcompare.py "$out/sw" "$out/hw" "$out/sheet.png"
