#!/bin/sh
# Downloads the pinned prebuilt AOSP Cuttlefish arm64 images (docs/DESIGN.md §5.10).
# The build ID lives in guest/BUILD_ID; never "latest".
set -eu
GUEST="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ID="$(cat "$GUEST/BUILD_ID")"
TARGET="${CF_TARGET:-aosp_cf_arm64_only_phone-userdebug}"
PRODUCT="${TARGET%-userdebug}"
BASE="https://ci.android.com/builds/submitted/$BUILD_ID/$TARGET/latest/raw"
OUT="$GUEST/out/android"
mkdir -p "$OUT"
fetch() {
  if [ -s "$OUT/$1" ]; then echo "have $1"; return; fi
  echo "fetching $1"
  curl -fL --progress-bar -o "$OUT/$1.part" "$BASE/$1"
  mv "$OUT/$1.part" "$OUT/$1"
}
fetch "$PRODUCT-img-$BUILD_ID.zip"
fetch BUILD_INFO
cd "$OUT" && unzip -o -q "$PRODUCT-img-$BUILD_ID.zip" && ls -la
