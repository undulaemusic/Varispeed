#!/bin/bash
# Measures the bridge's CPU use and latency for several IO buffer sizes (silent: output muted).
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/build"
for cfg in "64 128" "128 128" "128 256" "256 256" "256 512"; do
  set -- $cfg
  "$B/vsbridge" --mute --in-buffer "$1" --out-buffer "$2" --seconds 12 > "$B/buf.log" 2>&1 &
  BP=$!
  sleep 5
  tot=0
  for i in 1 2 3 4 5; do c=$(ps -o %cpu= -p $BP); tot=$(echo "$tot + $c" | bc); sleep 1; done
  wait $BP
  lat=$(awk 'NR>2 && $1 ~ /s$/ {l=$6} END {print l}' "$B/buf.log")
  printf "in %3s / out %3s frames: bridge CPU %5.1f%%   latency %s ms\n" "$1" "$2" "$(echo "$tot/5" | bc -l)" "$lat"
done
