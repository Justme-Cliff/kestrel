#!/bin/sh
# First-boot setup for Kestrel's Android over adb (docs/DESIGN.md §5.10 "customizing
# Android without rebuilding it"). Run after Android has booted.
#   usage: guest/firstboot/android-setup.sh [serial]   (default 127.0.0.1:6520)
set -eu
export ANDROID_SERIAL="${1:-127.0.0.1:6520}"
# adbd restarts once shortly after boot, dropping early connections: retry.
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
  adb connect "$ANDROID_SERIAL" >/dev/null 2>&1 || true
  sleep 3
done

# Games want an always-on, unlocked screen.
adb shell locksettings set-disabled true >/dev/null
adb shell settings put system screen_off_timeout 2147483647
adb shell svc power stayon true
adb shell input keyevent KEYCODE_WAKEUP
adb shell wm dismiss-keyguard

# Internet: Cuttlefish's virtual wifi, backed by Kestrel's NAT.
adb shell svc wifi enable
for _ in $(seq 1 30); do
  adb shell cmd wifi connect-network VirtWifi open >/dev/null 2>&1 || true
  adb shell ip -4 addr show wlan0 2>/dev/null | grep -q inet && break
  sleep 2
done
adb shell ip -4 addr show wlan0 | grep -q inet && echo "Android is online" || echo "wifi did not connect"
