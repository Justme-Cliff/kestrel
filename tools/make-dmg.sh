#!/bin/sh
# Builds Kestrel-<version>.dmg: drag Kestrel onto Applications to install.
#
#   tools/make-dmg.sh                 ad-hoc signed (fine for your own Macs and friends:
#                                     first launch is right-click > Open)
#   KESTREL_SIGN_ID="Developer ID Application: Name (TEAMID)" \
#   KESTREL_NOTARY_PROFILE=kestrel tools/make-dmg.sh
#                                     signed and notarized: opens with a double-click
#                                     anywhere (needs a paid Apple Developer account;
#                                     make the profile once with
#                                     `xcrun notarytool store-credentials kestrel`).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cmake --build "$ROOT/build"
APP="$ROOT/build/Kestrel.app"
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")
DMG="$ROOT/build/Kestrel-$VERSION.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

ditto "$APP" "$STAGE/Kestrel.app"
if [ -n "${KESTREL_SIGN_ID:-}" ]; then
  # Hardened runtime + the hypervisor entitlement: what notarization requires.
  codesign --force --deep --options runtime --timestamp --sign "$KESTREL_SIGN_ID" \
    --entitlements "$ROOT/kestrel.entitlements" "$STAGE/Kestrel.app"
fi
ln -s /Applications "$STAGE/Applications"
if [ -z "${KESTREL_SIGN_ID:-}" ]; then
  cat > "$STAGE/First launch.txt" <<'EOF'
Kestrel isn't notarized by Apple yet, so the first time:

  1. Drag Kestrel onto Applications.
  2. In Applications, right-click Kestrel and choose Open, then Open again.

After that it opens normally. Kestrel needs a Mac with Apple silicon and macOS 15 or later.
On first launch it downloads Android (about 1.5 GB).
EOF
fi

rm -f "$DMG"
hdiutil create -volname "Kestrel $VERSION" -srcfolder "$STAGE" -ov -format UDZO -fs APFS "$DMG" >/dev/null
if [ -n "${KESTREL_SIGN_ID:-}" ]; then
  codesign --sign "$KESTREL_SIGN_ID" --timestamp "$DMG"
  if [ -n "${KESTREL_NOTARY_PROFILE:-}" ]; then
    xcrun notarytool submit "$DMG" --keychain-profile "$KESTREL_NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
  fi
fi
echo "$DMG ($(du -h "$DMG" | cut -f1))"
