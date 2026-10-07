#!/bin/bash
# Evaluates bridge settings silently: (1) random speed changes for N s -> underruns/resyncs,
# (2) a slow 1 -> 0.5 -> 1 glide on a test tone -> pitch roughness (cents rms).
# Settings come from env (VS_SMOOTH, VS_CORR, VS_MAXCORR) and extra vsbridge args ("$@").
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="$ROOT/build"
if pgrep -x Varispeed >/dev/null; then echo "Varispeed app is running; quit it first"; exit 1; fi
N=${N:-40}
(VS_RANDOM=$N "$B/vssweep" > /dev/null 2>&1 &); sleep 1
"$B/vsbridge" --mute --seconds $N "$@" > "$B/eval_random.log"
U=$(tail -1 "$B/eval_random.log" | sed -E 's/.*underruns ([0-9]+).*resyncs ([0-9]+).*/underruns \1, resyncs \2/')
L=$(awk 'NR>2 && $1 ~ /s$/ && $3+0>0 {n++; l+=$6} END {printf "%.1f", l/n}' "$B/eval_random.log")
sleep 4
(VS_STEPS="1:0,0.5:3,1:3" "$B/vssweep" 5 > /dev/null 2>&1 &); sleep 2
"$B/vsbridge" --mute --seconds 14 --record "$B/eval_glide.wav" "$@" > "$B/eval_glide.log"
R=$(python3 -I "$ROOT/tools/analysis/pitch.py" "$B/eval_glide.wav" | head -1 | sed -E 's/.*roughness rms ([0-9.]+) cents, max ([0-9.]+).*/roughness \1 cents rms (max \2)/')
G=$(tail -1 "$B/eval_glide.log" | sed -E 's/.*underruns ([0-9]+).*/\1/')
sleep 3
echo "random: $U, mean latency $L ms | glide: $R, underruns $G"
