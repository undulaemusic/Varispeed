#!/bin/bash
# Steps the speed while Live plays and prints Live's real CPU use (from macOS) at each step,
# to compare with Live's own CPU meter.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; CTL="$ROOT/build/varispeedctl"
LIVE=$(pgrep -f "Ableton Live.*/Contents/MacOS/Live" | head -1)
[ -z "$LIVE" ] && { echo "Start Ableton Live and press play first."; exit 1; }
"$CTL" --ramp 0.3 1 > /dev/null; sleep 2
for s in 1 0.5 0.35 0.25 1; do
  "$CTL" "$s" > /dev/null
  printf ">>> Speed %3.0f%% - note Live's CPU meter now... " "$(echo "$s*100" | bc)"
  sleep 4
  tot=0; for i in 1 2 3 4 5 6 7 8; do c=$(ps -o %cpu= -p $LIVE); tot=$(echo "$tot + $c" | bc); sleep 1; done
  printf "Live's real CPU use: %5.1f%% of one core\n" "$(echo "$tot/8" | bc -l)"
done
"$CTL" --ramp 0.5 1 > /dev/null
echo "Done (speed back to 100%)."
