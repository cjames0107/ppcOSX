#!/bin/bash
# A disk image with ppcOSX.app and a link to Applications, to drag it to.
#
#   tools/make-dmg.sh path/to/ppcOSX.app OUTPUT.dmg
#
# The image is signed with SIGN_IDENTITY too (ad hoc by default).
set -euo pipefail
APP=${1:?usage: $0 path/to/ppcOSX.app OUTPUT.dmg}
OUT=${2:?usage: $0 path/to/ppcOSX.app OUTPUT.dmg}
ID=${SIGN_IDENTITY:--}
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/ppcOSX.app"
ln -s /Applications "$STAGE/Applications"
rm -f "$OUT"
hdiutil create -quiet -volname ppcOSX -srcfolder "$STAGE" -fs APFS \
    -format ULMO -o "$OUT"
codesign --force --sign "$ID" $([ "$ID" = "-" ] || echo --timestamp) "$OUT"
echo "disk image: $OUT ($(du -h "$OUT" | cut -f1))"
