#!/bin/bash
# Make a ppcOSX release: universal (Apple silicon and Intel), macOS 14
# Sonoma and later.
#
#   tools/release.sh
#
#   1. the libraries, for both architectures and merged
#      (tools/build-dist-deps.sh, when dist/prefix is empty)
#   2. the emulator against them, both architectures merged
#      (tools/build-dist-qemu.sh)
#   3. the app, Release, which bundles dist/qemu-build
#   4. licences, signing (SIGN_IDENTITY, ad hoc by default), disk image
#   5. with a Developer ID identity: notarization (tools/notarize.sh)
#   6. the matching source archive
#
# The results go in dist/release.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist/release"
ID=${SIGN_IDENTITY:--}
export DEVELOPER_DIR=${DEVELOPER_DIR:-$(xcode-select -p)}
VERSION=$(sed -n 's/.*MARKETING_VERSION = \(.*\);/\1/p' "$ROOT/app/ppcOSX.xcodeproj/project.pbxproj" | head -1)

ls "$ROOT/dist/prefix/lib/"*.dylib >/dev/null 2>&1 || "$ROOT/tools/build-dist-deps.sh"
"$ROOT/tools/build-dist-qemu.sh"

rm -rf "$OUT/ppcOSX.app" "$OUT/derived"
xcodebuild -quiet -project "$ROOT/app/ppcOSX.xcodeproj" -scheme ppcOSX \
    -configuration Release -derivedDataPath "$OUT/derived" \
    ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO QEMU_BUILD="$ROOT/dist/qemu-build" build
ditto "$OUT/derived/Build/Products/Release/ppcOSX.app" "$OUT/ppcOSX.app"
rm -rf "$OUT/derived"

"$ROOT/tools/collect-licenses.sh" "$OUT/ppcOSX.app"
"$ROOT/tools/sign-app.sh" "$OUT/ppcOSX.app"
"$ROOT/tools/make-dmg.sh" "$OUT/ppcOSX.app" "$OUT/ppcOSX-$VERSION.dmg"
if [ "$ID" != "-" ]; then
    "$ROOT/tools/notarize.sh" "$OUT/ppcOSX-$VERSION.dmg"
fi
"$ROOT/tools/make-source-archive.sh" "$OUT/ppcOSX-$VERSION-source.tar.xz"
echo "release $VERSION in $OUT"
