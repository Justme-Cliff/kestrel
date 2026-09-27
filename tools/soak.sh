#!/bin/sh
# Long stability test (default 3 hours) on a throwaway copy of your Android.
#
#   tools/soak.sh [--hours N | --minutes N] [--input] [--force] [--install-weekly]
#
# What it does:
#   - Clones ~/Library/Application Support/Kestrel/android into android-soaktest
#     (APFS clone: instant, no extra disk space; your own Android is never touched).
#   - Starts that copy in a Kestrel window, opens Free Fire and leaves it in the
#     lobby (no input to the game: no matches are played on your account).
#     With --input it stays on the home screen instead and a robot plays hard on
#     the keymapper (WASD, fast mouse flicks, taps) through the control socket.
#   - Every 5 minutes it resizes the window and toggles the toolbar (the kinds of
#     things that crashed before), and every minute it records memory, CPU, the
#     game's open file count and any crash, GPU stall or unexpected app close.
#   - Prints PASS or FAIL at the end; the minute-by-minute log is in
#     ~/Library/Logs/Kestrel/soak-<date>.log.
#
# Don't run it while playing: it's a second Android and would slow your game.
# --install-weekly adds a LaunchAgent that runs it every Sunday at 04:00
# (remove it with: launchctl unload ~/Library/LaunchAgents/com.kestrel.soak.plist).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
K="$ROOT/build/Kestrel.app/Contents/MacOS/kestrel"
SUPPORT="$HOME/Library/Application Support/Kestrel"
LOGS="$HOME/Library/Logs/Kestrel"
MINUTES=180
INPUT=0
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --hours) MINUTES=$(($2 * 60)); shift ;;
    --minutes) MINUTES=$2; shift ;;
    --input) INPUT=1 ;;
    --force) FORCE=1 ;;  # run even though Kestrel is open (slows it down)
    --install-weekly)
      PLIST="$HOME/Library/LaunchAgents/com.kestrel.soak.plist"
      mkdir -p "$HOME/Library/LaunchAgents"
      cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.kestrel.soak</string>
  <key>ProgramArguments</key><array><string>$ROOT/tools/soak.sh</string></array>
  <key>StartCalendarInterval</key><dict><key>Weekday</key><integer>0</integer><key>Hour</key><integer>4</integer><key>Minute</key><integer>0</integer></dict>
  <key>StandardOutPath</key><string>$LOGS/soak-weekly.out</string>
  <key>StandardErrorPath</key><string>$LOGS/soak-weekly.out</string>
</dict></plist>
EOF
      launchctl unload "$PLIST" 2>/dev/null
      launchctl load "$PLIST" && echo "Weekly soak installed (Sundays 04:00)."
      exit 0 ;;
    *) echo "usage: $0 [--hours N | --minutes N] [--input] [--force] [--install-weekly]"; exit 2 ;;
  esac
  shift
done

if [ $FORCE = 0 ] && pgrep -f "Kestrel.app/Contents/MacOS/kestrel$" >/dev/null; then
  echo "Kestrel is running: quit it first (the test runs a second Android)."; exit 1
fi
[ -x "$K" ] || { echo "Build first: cmake --build build"; exit 1; }

STAMP=$(date +%Y-%m-%d-%H%M)
LOG="$LOGS/soak-$STAMP.log"
SOCK="/tmp/kestrel-soak-$$.sock"
mkdir -p "$LOGS"

# Fresh copy of your Android (images and data).
TEST="$SUPPORT/android-soaktest"
rm -rf "$TEST"; mkdir -p "$TEST/instance"
for f in "$SUPPORT/android"/*; do [ -f "$f" ] && cp -c "$f" "$TEST/"; done
for f in userdata.img metadata.img misc.img frp.img super.raw; do cp -c "$SUPPORT/android/instance/$f" "$TEST/instance/"; done

crashes0=$(ls "$HOME/Library/Logs/DiagnosticReports" | grep -c '^kestrel-')
stalls0=$(ls "$LOGS" | grep -c '^gpu-stall-')
closes0=$(ls "$LOGS" | grep -c '^app-closed-')

"$K" --android "$TEST" --ram 3072 --low-latency --control "$SOCK" > "$LOGS/soak-$STAMP.console.log" 2>&1 &
KP=$!
# This Kestrel's own adb port (6520, or the next one if another Kestrel has it).
t=0; PORT=""; until [ -n "$PORT" ] || [ $t -gt 30 ]; do
  PORT=$(lsof -nP -a -p $KP -iTCP -sTCP:LISTEN 2>/dev/null | grep -o '127.0.0.1:[0-9]*' | head -1 | cut -d: -f2)
  sleep 1; t=$((t + 1))
done
adb_shell() { KESTREL_ADB_PORT=$PORT "$ROOT/build/kestrel" adb shell "$@" 2>/dev/null | tr -d '\r'; }
t=0; until adb_shell getprop sys.boot_completed | grep -q 1 || [ $t -gt 150 ]; do sleep 3; t=$((t + 3)); done
echo "$(date +%T) booted in ${t}s" | tee -a "$LOG"
sleep 5

ROBOT=""
if [ $INPUT = 1 ]; then
  printf 'foreground com.dts.freefireth\n' | nc -U -w2 "$SOCK" >/dev/null
  python3 "$ROOT/tools/soak_robot.py" "$SOCK" "$MINUTES" >> "$LOG" 2>&1 &
  ROBOT=$!
else
  adb_shell monkey -p com.dts.freefireth -c android.intent.category.LAUNCHER 1 >/dev/null
fi

FAIL=""
for m in $(seq 1 $MINUTES); do
  sleep 60
  if ! kill -0 $KP 2>/dev/null; then FAIL="Kestrel exited at minute $m"; echo "$(date +%T) $FAIL" >> "$LOG"; break; fi
  info=$(adb_shell 'F=$(pidof com.dts.freefireth); echo "ff=$F $(grep FDSize /proc/$F/status 2>/dev/null | tr -d "\t") avail=$(grep MemAvailable /proc/meminfo | tr -s " " | cut -d" " -f2)kB"')
  rss=$(( $(ps -o rss= -p $KP) / 1024 )); cpu=$(ps -o %cpu= -p $KP | tr -d ' ')
  echo "$(date +%T) m=$m rss=${rss}MB cpu=$cpu $info" >> "$LOG"
  if [ $((m % 5)) -eq 0 ]; then
    for i in 1 2 3; do
      osascript -e "tell application \"System Events\" to tell (first process whose unix id is $KP) to click menu item \"Show/Hide Toolbar\" of menu \"Window\" of menu bar 1" >/dev/null 2>&1; sleep 1
      osascript -e "tell application \"System Events\" to tell (first process whose unix id is $KP) to set size of window 1 to {$((900 + i * 80)), $((560 + i * 45))}" >/dev/null 2>&1; sleep 1
    done
  fi
done

[ -n "$ROBOT" ] && kill $ROBOT 2>/dev/null
kill $KP 2>/dev/null; wait $KP 2>/dev/null
crashes=$(( $(ls "$HOME/Library/Logs/DiagnosticReports" | grep -c '^kestrel-') - crashes0 ))
stalls=$(( $(ls "$LOGS" | grep -c '^gpu-stall-') - stalls0 ))
closes=$(( $(ls "$LOGS" | grep -c '^app-closed-') - closes0 ))
fd_first=$(grep -o 'FDSize:[0-9]*' "$LOG" | head -1 | cut -d: -f2)
fd_last=$(grep -o 'FDSize:[0-9]*' "$LOG" | tail -1 | cut -d: -f2)
[ $crashes -gt 0 ] && FAIL="$FAIL $crashes Kestrel crash(es)"
[ $stalls -gt 0 ] && FAIL="$FAIL $stalls GPU stall(s)"
[ $closes -gt 0 ] && FAIL="$FAIL $closes unexpected app close(s)"
[ -n "$fd_first" ] && [ -n "$fd_last" ] && [ "$fd_last" -gt $((fd_first * 2)) ] && FAIL="$FAIL file handles grew $fd_first -> $fd_last"
rm -rf "$TEST" "$SOCK"
if [ -z "$FAIL" ]; then echo "PASS: $MINUTES min, no crashes, stalls or app closes (log: $LOG)" | tee -a "$LOG"; exit 0; fi
echo "FAIL:$FAIL (log: $LOG)" | tee -a "$LOG"; exit 1
