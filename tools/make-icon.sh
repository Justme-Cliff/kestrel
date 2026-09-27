#!/bin/sh
# Renders assets/icon/kestrel.svg into assets/Kestrel.icns (AppKit draws the SVG).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
swiftc -O "$root/tools/render-svg.swift" -o "$tmp/render"
set=$tmp/Kestrel.iconset
mkdir "$set"
for s in 16 32 128 256 512; do
  "$tmp/render" "$root/assets/icon/kestrel.svg" "$set/icon_${s}x${s}.png" $s
  "$tmp/render" "$root/assets/icon/kestrel.svg" "$set/icon_${s}x${s}@2x.png" $((s * 2))
done
iconutil -c icns "$set" -o "$root/assets/Kestrel.icns"
