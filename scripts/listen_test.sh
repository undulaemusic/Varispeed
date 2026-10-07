#!/bin/bash
# Milestone 5 listening test: runs the bridge (Varispeed -> default output, channels 1-2) and walks
# through a set of speeds with smooth glides. Start playback in Live (output = Varispeed) first.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CTL="$ROOT/build/varispeedctl"
"$CTL" --ramp 1 1 >/dev/null
"$ROOT/build/vsbridge" "$@" > "$ROOT/build/listen_bridge.log" 2>&1 &
BRIDGE=$!
trap 'kill $BRIDGE 2>/dev/null; "$CTL" --ramp 0.5 1 >/dev/null' EXIT
sleep 2
for s in 1 0.9 0.75 0.5 0.25 1 1.5 2 1; do
  printf "\n>>> Speed %s  " "$s"; "$CTL" "$s" | sed 's/^/(/; s/$/)/'
  sleep 8
done
kill -INT $BRIDGE; sleep 1
echo; tail -1 "$ROOT/build/listen_bridge.log"
