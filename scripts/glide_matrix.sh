#!/bin/bash
# Glitch test across DAW buffer sizes: a busy simulated DAW does quick glides while the bridge
# runs muted. Counts clock jumps (glitches) the DAW sees. Quit the Varispeed app first.
# Usage: glide_matrix.sh [buffers...]   (default: 32 64 128 256 512)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/build"
if pgrep -x Varispeed >/dev/null; then echo "Varispeed app is running; quit it first"; exit 1; fi
run() {   # buffer load steps -> discontinuities
  VS_LOAD=$2 VS_BUFFER=$1 VS_STEPS="$3" "$B/vssweep" 2.5 2>&1 | tail -1 | sed -E 's/.*discontinuities: ([0-9]+).*/\1/'
}
"$B/vsbridge" --mute --in-buffer 512 --seconds 600 > /dev/null 2>&1 &
BP=$!; trap 'kill $BP 2>/dev/null' EXIT; sleep 1
for buf in ${@:-32 64 128 256 512}; do
  a=$(run $buf 0.6 "0.5:0,1:0,0.5:2,1:0,0.5:2,1:0")
  b=$(run $buf 0.6 "1:0,0.5:0,1:2,0.5:0,1:2,0.5:0")
  c=$(run $buf 0.3 "1:0,2:0,1:2,2:0,1:2,2:0")
  d=$(run $buf 0.3 "2:0,1:0,2:2,1:0,2:2,1:0")
  printf "buffer %3s: 50->100%% ups %4s | 100->50%% downs %4s | 100->200%% ups %4s | 200->100%% downs %4s\n" "$buf" "$a" "$b" "$c" "$d"
done
