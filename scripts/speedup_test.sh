#!/bin/bash
# Speed-up glitch test: a busy simulated DAW (VS_LOAD, default 0.6) at a given buffer size does
# repeated fast speed-ups while the bridge runs muted. Prints clock jumps seen by the DAW and
# glitches in the DAW's audio as the bridge receives it.
# Usage: speedup_test.sh BUFFER [MAX_RISE_ST_PER_PERIOD]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/build"
if pgrep -x Varispeed >/dev/null; then echo "Varispeed app is running; quit it first"; exit 1; fi
BUF=$1; RISE=${2:-}
[ -n "$RISE" ] && export VS_MAXRISE=$RISE
"$B/vsbridge" --mute --in-buffer 256 --seconds 22 > "$B/speedup_bridge.log" 2>&1 &
BP=$!; sleep 1
OUT=$(VS_LOAD=${VS_LOAD:-0.6} VS_BUFFER=$BUF VS_STEPS="0.5:0,1:0,0.5:0,2:0,1:0,2:0,0.5:0" "$B/vssweep" 2.5 2>&1 | tail -1)
wait $BP
G=$(tail -1 "$B/speedup_bridge.log" | sed -E 's/.*glitches out [0-9]+ \/ in ([0-9]+).*/\1/')
printf "buffer %4s, max rise %-4s: %s | DAW-audio glitches: %s\n" "$BUF" "${RISE:-0.5}" "$(echo "$OUT" | sed -E 's/IO cycles: [0-9]+ +//')" "$G"
