#!/bin/sh
# Boots Android, installs an APK and writes a file, reboots, and checks both are
# still there. Needs the Cuttlefish images (guest/scripts/fetch-android.sh), adb,
# and an APK: tests/integration/android_persistence.sh <apk>. Takes ~4 minutes.
set -u
APK="$1"
cd "$(dirname "$0")/../.."
LOGDIR="$(mktemp -d)"
export ANDROID_SERIAL=127.0.0.1:6520
L=guest/out/android/instance/logcat.txt
boot() {
  ./build/kestrel --android guest/out/android --headless --keep-data < /dev/null > "$LOGDIR/persist_$1.log" 2>&1 &
  sleep 3
  until LC_ALL=C grep -a -q "Boot is finished" $L 2>/dev/null; do sleep 3; done
  sleep 5
  # adbd restarts once after boot: retry until a connection sticks.
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
    adb connect 127.0.0.1:6520 >/dev/null 2>&1; sleep 3
  done
}
boot A
adb install "$APK" | tail -1
adb shell 'echo kestrel-persist-test > /sdcard/Download/marker.txt; sync'
adb disconnect >/dev/null; pkill -INT kestrel; sleep 6
boot B
PKG=$(adb shell pm list packages | tr -d '\r' | grep -c .)
MARKER=$(adb shell cat /sdcard/Download/marker.txt | tr -d '\r')
adb disconnect >/dev/null; pkill -INT kestrel; sleep 3
[ "$MARKER" = "kestrel-persist-test" ] && [ "$PKG" -gt 0 ] && echo "PASS: data survived reboot" && exit 0
echo "FAIL: marker='$MARKER'"; exit 1
