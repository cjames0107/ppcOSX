#!/bin/bash
# Sign ppcOSX.app for distribution, inside out: the bundled libraries, the
# emulator (with the JIT entitlement TCG needs), then the app. Everything
# gets the hardened runtime, which notarization requires.
#
#   tools/sign-app.sh path/to/ppcOSX.app
#
# SIGN_IDENTITY picks the certificate: "-" (the default) signs ad hoc, which
# runs on this Mac and, after the user approves it in Privacy & Security,
# on others. With a "Developer ID Application: …" identity the signatures
# carry a secure timestamp, ready for tools/notarize.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=${1:?usage: $0 path/to/ppcOSX.app}
ID=${SIGN_IDENTITY:--}
ENT="$ROOT/app/Signing"

opts=(--force --options runtime --sign "$ID")
[ "$ID" = "-" ] || opts+=(--timestamp)

for l in "$APP/Contents/Frameworks/"*.dylib; do
    codesign "${opts[@]}" "$l"
done
# Library validation lets a process load only libraries with its own Team
# ID; an ad hoc signature has none, so ad hoc QEMU is exempted from it.
qemu_ent="$ENT/qemu.entitlements"
if [ "$ID" = "-" ]; then
    qemu_ent=$(mktemp); trap 'rm -f "$qemu_ent"' EXIT
    cp "$ENT/qemu.entitlements" "$qemu_ent"
    /usr/libexec/PlistBuddy -c "Add :com.apple.security.cs.disable-library-validation bool true" "$qemu_ent"
fi
for b in "$APP/Contents/Resources/qemu/bin/"*; do
    codesign "${opts[@]}" --entitlements "$qemu_ent" "$b"
done
codesign "${opts[@]}" --entitlements "$ENT/app.entitlements" "$APP"

codesign --verify --deep --strict "$APP"
echo "signed $APP ($([ "$ID" = "-" ] && echo "ad hoc" || echo "$ID"))"
