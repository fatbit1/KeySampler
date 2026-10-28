"""Which LFO moved the sound? Prints: amp, balance, pitch, bright spreads over 50 ms windows.
usage: lfo_check.py out.wav start_s expect(vol|flt|pit|pan|none)"""
import sys, wave, numpy as np
w = wave.open(sys.argv[1]); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2) / 32768.0
t0 = float(sys.argv[2]); exp = sys.argv[3]
amp, bal, zc, br = [], [], [], []
for k in range(30):
    a = int((t0 + 0.6 + k * 0.05) * sr); seg = x[a:a + int(0.05 * sr)]
    l, r = np.sqrt((seg ** 2).mean(0)); m = seg.mean(1)
    amp.append(l + r); bal.append((l - r) / (l + r + 1e-9))
    up = np.where((m[:-1] < 0) & (m[1:] >= 0))[0]
    fr = up + m[up] / (m[up] - m[up + 1])           # interpolated rising zero crossings
    zc.append((len(fr) - 1) / (fr[-1] - fr[0]) if len(fr) > 2 else 0)
    br.append(np.sqrt((np.diff(m) ** 2).mean()) / (np.sqrt((m ** 2).mean()) + 1e-9))
sp = lambda v: (max(v) - min(v)) / (abs(np.mean(v)) + 1e-9)
r = {"vol": sp(amp), "pan": max(bal) - min(bal), "pit": sp(zc), "flt": sp(br)}
print(" ".join(f"{k}={v:.3f}" for k, v in r.items()))
# each LFO leaves a clear mark on a filtered saw (measured: baseline < 0.07 everywhere)
rules = {
    "vol": r["vol"] > 1.0 and r["pan"] < 0.1,
    "pan": r["pan"] > 1.0,
    "flt": r["flt"] > 0.5 and r["vol"] < 1.0 and r["pan"] < 0.1,
    "pit": r["pit"] > 0.05 and r["vol"] < 0.2 and r["flt"] < 0.3 and r["pan"] < 0.1,
}
found = [k for k, v in rules.items() if v]
print("moved:", found); sys.exit(0 if found == ([exp] if exp != "none" else []) else 1)
