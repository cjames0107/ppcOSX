#!/bin/bash
# Put the licences of everything ppcOSX.app ships into its
# Contents/Resources/Licenses, with a note on getting the source code
# (QEMU and the LGPL libraries require it be offered).
#
#   tools/collect-licenses.sh path/to/ppcOSX.app
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=${1:?usage: $0 path/to/ppcOSX.app}
OUT="$APP/Contents/Resources/Licenses"
SRC="$ROOT/dist/src"
rm -rf "$OUT"; mkdir -p "$OUT"

src() { ls -d "$SRC/$1/unpacked/"*/ | head -1; }
put() {   # put NAME FILE...: the files from that source, as NAME[-file]
    local name=$1 dir; shift
    dir=$(src "$name")
    for f in "$@"; do
        if [ $# -eq 1 ]; then cp "$dir/$f" "$OUT/$name.txt"
        else cp "$dir/$f" "$OUT/$name-$f.txt"; fi
    done
}

cp "$ROOT/qemu/COPYING" "$OUT/QEMU-GPL-2.0.txt"
cp "$ROOT/qemu/COPYING.LIB" "$OUT/QEMU-LGPL-2.1.txt"
cp "$ROOT/qemu/LICENSE" "$OUT/QEMU-LICENSE.txt"
cp "$ROOT/qemu/roms/openbios/COPYING" "$OUT/OpenBIOS.txt"
put capstone LICENSE.TXT
put glib COPYING
put gmp COPYING.LESSERv3 COPYINGv2
put gnutls COPYING.LESSERv2
put libjpeg-turbo LICENSE.md
put libpng LICENSE
put libslirp COPYRIGHT
put libssh COPYING
put libtasn1 COPYING.LESSERv2
put libusb COPYING
put nettle COPYING.LESSERv3 COPYINGv2
put openssl LICENSE.txt
put pixman COPYING
put zstd LICENSE
g=$(src glib)
cp "$g/subprojects/pcre2-10.44/LICENCE" "$OUT/pcre2.txt"
cp "$g/subprojects/proxy-libintl/COPYING" "$OUT/proxy-libintl.txt"

cat > "$OUT/README.txt" <<TXT
ppcOSX includes the following software, each under its own licence (the
files in this folder):

  QEMU (with ppcOSX's changes)   GPL 2.0      QEMU-*.txt
  OpenBIOS                       GPL 2.0      OpenBIOS.txt
  GLib, proxy-libintl            LGPL 2.1     glib.txt, proxy-libintl.txt
  PCRE2                          BSD          pcre2.txt
  GnuTLS                         LGPL 2.1     gnutls-*.txt
  GMP, Nettle                    LGPL 3 / GPL 2   gmp-*.txt, nettle-*.txt
  libtasn1                       LGPL 2.1     libtasn1-*.txt
  libssh                         LGPL 2.1     libssh.txt
  libusb                         LGPL 2.1     libusb.txt
  OpenSSL                        Apache 2.0   openssl.txt
  pixman                         MIT          pixman.txt
  libslirp                       BSD          libslirp.txt
  Capstone                       BSD          capstone.txt
  libpng                         libpng       libpng.txt
  libjpeg-turbo                  IJG / BSD    libjpeg-turbo.txt
  Zstandard                      BSD          zstd.txt

The libraries are in Contents/Frameworks as separate dynamic libraries,
which you may replace with your own builds of the same versions.

Source code: the complete source of this build (the QEMU fork, the ppcOSX
app and the build scripts, and the exact source archives of every library
above) is available from whoever gave you this copy of ppcOSX, for at least
three years from the date of this build ($(date +%Y-%m-%d)).
It is made with tools/make-source-archive.sh.
TXT
echo "licences: $(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
