#!/bin/bash
# The corresponding source of a ppcOSX release, for the GPL and LGPL:
# the QEMU fork, the app, the scripts, and the library archives exactly
# as downloaded by build-dist-deps.sh.
#
#   tools/make-source-archive.sh [OUTPUT.tar.xz]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/.*MARKETING_VERSION = \(.*\);/\1/p' "$ROOT/app/ppcOSX.xcodeproj/project.pbxproj" | head -1)
OUT=${1:-$ROOT/dist/release/ppcOSX-$VERSION-source.tar.xz}
mkdir -p "$(dirname "$OUT")"
LIST=$(mktemp); trap 'rm -f "$LIST"' EXIT
cd "$ROOT"
{
    echo README.md; echo ppcosx; echo .gitignore
    find app tools guest tests -type f ! -name .DS_Store ! -path '*/xcuserdata/*' \
        ! -path 'app/build/*' 2>/dev/null
    find qemu -type f ! -path 'qemu/.git/*' ! -name .DS_Store ! -path 'qemu/build/*'
    find dist/src -mindepth 2 -maxdepth 2 -type f ! -name '*.part'
} | sort > "$LIST"
tar -c -J -f "$OUT" -s ",^,ppcOSX-$VERSION/," -T "$LIST"
echo "source: $OUT ($(du -h "$OUT" | cut -f1))"
