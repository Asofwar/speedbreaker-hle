#!/usr/bin/env python3
"""Sample the guest's static data (0x82A00000-0x82CD0000) every 0.5 s for N seconds and report words that took only a few small values (state variables), with when they changed."""
import sys, time, struct, os
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import gmem
N = float(sys.argv[1]) if len(sys.argv) > 1 else 60
p = gmem.pid(); m = open('/proc/%d/mem' % p, 'rb', 0)
LO, HI = 0x82A00000, 0x82CD0000
def snap():
    m.seek(gmem.BASE + LO); return np.frombuffer(m.read(HI - LO), '>u4').copy()
t0 = time.time(); samples = []; times = []
while time.time() - t0 < N:
    samples.append(snap()); times.append(time.time()); time.sleep(0.3)
A = np.stack(samples)           # [t, words]
changes = (A[1:] != A[:-1]).sum(0)
cand = np.nonzero((changes >= 1) & (changes <= 8))[0]
print(len(samples), 'samples;', len(cand), 'words changed 1-8 times')
out = []
for i in cand:
    col = A[:, i]
    vals = sorted(set(int(v) for v in col))
    if max(vals) > 64 or len(vals) > 6: continue          # small enum-like values only
    ts = [round(times[k + 1] - t0, 1) for k in np.nonzero(col[1:] != col[:-1])[0]]
    out.append((LO + int(i) * 4, vals, ts))
for a, v, ts in out[:150]:
    print('%08X values %s at t=%s' % (a, v, ts))
print(len(out), 'enum-like words')
print('epoch0 %.1f' % t0)
