"""Pitch-smoothness analysis of a recorded test-tone glide (32-bit float stereo WAV from VSRecorder).

Tracks the tone's frequency every cycle (interpolated zero crossings) and reports how rough
the glide is: each cycle's deviation from a +-25 ms local average, in cents.
Usage: python3 -I pitch.py file.wav [t_start t_end]
"""
import array, math, struct, sys

d = open(sys.argv[1], 'rb').read()
rate = struct.unpack('<I', d[24:28])[0]
x = array.array('f'); x.frombytes(d[58:]); L = list(x[0::2])
t0, t1 = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else (3.0, 9.0)
zc = [(i - 1 + (-L[i-1]) / (L[i] - L[i-1])) / rate for i in range(1, len(L)) if L[i-1] < 0 <= L[i]]
f = [(0.5 * (zc[i] + zc[i+1]), 1 / (zc[i+1] - zc[i])) for i in range(len(zc) - 1)]
seg = [(t, v) for t, v in f if t0 < t < t1 and 200 < v < 900]
cents = []
for t, v in seg:
    w = [vv for tt, vv in seg if abs(tt - t) < 0.025]
    cents.append(1200 * math.log2(v / (sum(w) / len(w))))
print(f"rate {rate}, cycles {len(seg)}, roughness rms {math.sqrt(sum(c*c for c in cents)/len(cents)):.2f} cents, max {max(abs(c) for c in cents):.2f} cents")
worst = sorted(range(len(cents)), key=lambda i: -abs(cents[i]))[:8]
for i in sorted(worst): print(f"  {seg[i][0]:7.3f}s {seg[i][1]:8.2f} Hz  {cents[i]:+7.2f} cents")
