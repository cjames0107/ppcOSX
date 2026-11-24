#!/bin/bash
# Notarize a Developer ID-signed ppcOSX disk image and staple the ticket,
# so it opens on other Macs without a warning.
#
#   tools/notarize.sh dist/release/ppcOSX-0.1.dmg
#
# The credentials come from a notarytool keychain profile, made once with
#   xcrun notarytool store-credentials ppcosx-notary \
#       --apple-id YOU@EXAMPLE.COM --team-id TEAMID
# (NOTARY_PROFILE names another profile.)
set -euo pipefail
DMG=${1:?usage: $0 path/to/ppcOSX.dmg}
PROFILE=${NOTARY_PROFILE:-ppcosx-notary}

if codesign -dv "$DMG" 2>&1 | grep -q "Signature=adhoc"; then
    echo "$DMG is signed ad hoc: Apple notarizes only Developer ID signatures" >&2
    exit 1
fi
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
