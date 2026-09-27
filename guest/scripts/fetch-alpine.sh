#!/bin/sh
# Downloads the prebuilt Alpine Linux aarch64 "virt" netboot kernel + initramfs
# used to bring up the VMM (docs/DESIGN.md §5.10). Checks SHA-256 once pinned.
set -eu
VERSION="${ALPINE_VERSION:-3.22}"
BASE="https://dl-cdn.alpinelinux.org/alpine/v${VERSION}/releases/aarch64/netboot"
OUT="$(cd "$(dirname "$0")/.." && pwd)/out"
mkdir -p "$OUT"
for f in vmlinuz-virt initramfs-virt; do
  echo "fetching $f"
  curl -fL --progress-bar -o "$OUT/$f" "$BASE/$f"
done
SUMS="$(cd "$(dirname "$0")" && pwd)/alpine.sha256"
if [ -f "$SUMS" ]; then
  (cd "$OUT" && shasum -a 256 -c "$SUMS")
else
  (cd "$OUT" && shasum -a 256 vmlinuz-virt initramfs-virt) > "$SUMS"
  echo "pinned checksums in $SUMS"
fi
