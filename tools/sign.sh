#!/bin/sh
# sign.sh <file or .app> <entitlements> [--deep]
# Signs a copy outside the project folder and copies it back. The project lives on the
# Desktop, which iCloud syncs: files there carry file-provider metadata that codesign
# rejects ("resource fork, Finder information, or similar detritus not allowed").
set -e
TARGET=$1 ENT=$2 DEEP=${3:-}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
NAME=$(basename "$TARGET")
ditto --norsrc --noextattr "$TARGET" "$TMP/$NAME"
codesign --force $DEEP --sign - --entitlements "$ENT" "$TMP/$NAME"
rm -rf "$TARGET.signing"
ditto "$TMP/$NAME" "$TARGET.signing"  # keep attributes: they hold signatures of non-code files
rm -rf "$TARGET"
mv "$TARGET.signing" "$TARGET"   # atomic: a running copy keeps its old file
