#!/bin/sh
# Boots the prebuilt Alpine guest, runs a command in its shell, powers it off,
# and checks the output. Needs guest/scripts/fetch-alpine.sh to have been run.
set -eu
KESTREL="$1"
OUT_DIR="$(cd "$(dirname "$0")/../../guest/out" && pwd)"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
(sleep 8; printf 'uname -m\n'; sleep 1; printf 'echo o > /proc/sysrq-trigger\n'; sleep 5) |
  "$KESTREL" --kernel "$OUT_DIR/vmlinuz-virt" --initrd "$OUT_DIR/initramfs-virt" --ram 512 >"$LOG" 2>&1
for expected in "Machine model: Kestrel virtual machine" "Run /init as init process" \
                "aarch64" "reboot: Power down" "guest powered off"; do
  if ! grep -q "$expected" "$LOG"; then
    echo "missing: $expected"; tail -30 "$LOG"; exit 1
  fi
done
echo "Alpine booted, ran a command and powered off"
