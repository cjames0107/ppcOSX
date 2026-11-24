#!/bin/sh
# Build the guest components inside the running development guest and copy
# the results into guest/prebuilt/<Mac OS X version>/, so install.sh can
# install them on guests that have no compiler.
#
#   tools/build-guest-binaries.sh
#
# Needs the development guest running (tools/run-tiger-dev.sh) with Xcode
# Tools installed. The binaries only suit the Mac OS X version they were
# built on.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
ssh_guest="$root/tools/guest-ssh.sh"

"$root/tools/guest-sync.sh"
version=$("$ssh_guest" 'sw_vers -productVersion' | cut -d. -f1-2)
case "$version" in
10.[0-9]) ;;
*) echo "build-guest-binaries: unexpected guest version '$version'" >&2; exit 1 ;;
esac
out="$root/guest/prebuilt/$version"

# Clean builds, so nothing stale from earlier work ships.
"$ssh_guest" 'cd ~/src &&
    for d in PPCOSXFramebuffer libppcosxgl PPCOSXGL; do
        rm -rf $d/build && make -C $d >/dev/null || exit 1
    done &&
    rm -rf ~/prebuilt && mkdir ~/prebuilt &&
    cp -R PPCOSXFramebuffer/build/PPCOSXFramebuffer.kext ~/prebuilt/ &&
    cp PPCOSXGL/build/libPPCOSXGL.dylib ~/prebuilt/ &&
    { echo "built on Mac OS X $(sw_vers -productVersion) ($(sw_vers -buildVersion))";
      echo "compiler: $(gcc --version | head -1)";
      echo "date: $(date -u "+%Y-%m-%d %H:%M UTC")"; } > ~/prebuilt/BUILDINFO'

rm -rf "$out"
mkdir -p "$out"
"$ssh_guest" 'tar -C ~/prebuilt -cf - .' | tar -C "$out" -xf -
echo "$out:"
cat "$out/BUILDINFO"
