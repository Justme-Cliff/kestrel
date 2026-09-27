#!/bin/sh
# Fetches ANGLE (OpenGL ES on Metal, BSD-3-Clause) for host GLES rendering.
# Taken from a pinned, checksummed Electron release, which ships Chromium's ANGLE
# build as two standalone dylibs.
set -eu
out="${1:?usage: fetch-angle.sh <out-dir>}"
[ -f "$out/libEGL.dylib" ] && [ -f "$out/libGLESv2.dylib" ] && exit 0
ver=43.6.0
sha=5183a2b15d013517386edd9f1ea8e3402755f6c3ea17893f92acb20052f3a2e7
zip="electron-v$ver-darwin-arm64.zip"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fL --retry 3 -o "$tmp/$zip" "https://github.com/electron/electron/releases/download/v$ver/$zip"
echo "$sha  $tmp/$zip" | shasum -a 256 -c -
lib='Electron.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries'
unzip -q -j "$tmp/$zip" "$lib/libEGL.dylib" "$lib/libGLESv2.dylib" -d "$tmp/angle"
mkdir -p "$out"
mv "$tmp/angle/libEGL.dylib" "$tmp/angle/libGLESv2.dylib" "$out/"
