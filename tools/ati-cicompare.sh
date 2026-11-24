#!/bin/sh
# Run the guest/tests/cicompare Core Image filters on the GPU (Apple's ATI
# driver) and in Core Image's software renderer, in a guest started by
# tools/ati-probe.sh or tools/ati-probe-r300.sh (ATI_KEEP=1), and compare.
#
#   tools/ati-cicompare.sh LABEL [SIZE]
#
# SIZE is the picture's width and height (default 128).
# Pictures, timings and a contact sheet (sheet.png: software, GPU,
# difference) go to vms/ati-probe/LABEL/ci/.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
out="vms/ati-probe/$1/ci${2:+-$2}"
size=${2:-128}
rm -rf "$out"
mkdir -p "$out"
# ATI_BINS=DIR: a prebuilt DIR/cicompare-SIZE (for guests with no compiler)
if [ -n "${ATI_BINS:-}" ]; then
    tools/guest-ssh.sh 'cat > /tmp/cicompare && chmod +x /tmp/cicompare' \
        < "$ATI_BINS/cicompare-$size"
else
    tools/guest-ssh.sh 'cat > /tmp/cicompare.m && cd /tmp &&
        gcc -DIW='"$size"' -o cicompare cicompare.m -framework Foundation -framework QuartzCore \
            -framework OpenGL -framework ApplicationServices 2>&1 |
            grep -v warning | grep -v "^cicompare.m: In"' \
        < guest/tests/cicompare.m
fi
tools/guest-ssh.sh 'cd /tmp && rm -rf hw sw && mkdir hw sw &&
    ./cicompare /tmp/hw hw > hw/times.txt 2> hw/err.txt;
    ./cicompare /tmp/sw sw > sw/times.txt 2> sw/err.txt;
    tar cf - hw sw' | tar xf - -C "$out"
cat "$out/hw/err.txt" "$out/sw/err.txt"
paste "$out/sw/times.txt" "$out/hw/times.txt" |
    awk '{ printf "%-28s sw %8s ms   gpu %8s ms\n", $1, $2, $5 }'
python3 tools/glcompare.py "$out/sw" "$out/hw" "$out/sheet.png"
