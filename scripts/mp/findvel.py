#!/usr/bin/env python3
"""Find the player's rigid body: while the car drives, a float triple that moves by D between two snapshots dt apart, with a velocity triple ~ D/dt
within 256 bytes of it. Sends '1 RT 8' first."""
import sys, time, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import gmem
p = gmem.pid()
open('/tmp/ao/pad.cmd', 'a').write('1 RT 9.0\n'); time.sleep(4.0)
t1 = time.time(); s1 = gmem.snapshot(p); t1b = time.time()
s2 = gmem.snapshot(p); t2 = time.time()
dt = (t2 - (t2 - t1b) / 2 * 0) - (t1 + t1b) / 2   # mid of S1 to the end of S2 read: approximate, refine below
tm1 = (t1 + t1b) / 2; tm2 = t1b + (t2 - t1b) / 2
dt = tm2 - tm1
print('dt %.2f s' % dt, file=sys.stderr)
out = []
for start, b1 in s1.items():
    b2 = s2.get(start)
    if b2 is None or len(b2) != len(b1): continue
    n = len(b1) // 4
    a1 = np.frombuffer(b1, '>f4', n); a2 = np.frombuffer(b2, '>f4', n)
    with np.errstate(all='ignore'):
        fin = np.isfinite(a1) & np.isfinite(a2) & (np.abs(a1) < 1e5) & (np.abs(a2) < 1e5)
        d = a2 - a1
        # position candidates: triple whose 3 components are finite and displacement magnitude 3..80
        D = np.stack([d[:-2], d[1:-1], d[2:]])
        mag = np.sqrt((D ** 2).sum(0))
        ok = fin[:-2] & fin[1:-1] & fin[2:] & (mag > 3) & (mag < 80)
        # exclude zero-padded junk: at least 2 components moved
        ok &= ((np.abs(D) > 0.2).sum(0) >= 2)
        big = np.maximum(np.maximum(np.abs(a2[:-2]), np.abs(a2[1:-1])), np.abs(a2[2:]))
        ok &= (big > 50) & (np.minimum(np.abs(a2[:-2]), np.abs(a2[2:])) > 0.01)
    for i in np.nonzero(ok)[0]:
        V = D[:, i] / dt
        # look for a velocity triple at i+k (k words, |k| <= 64), in S2, matching V within 30% + 1.0
        lo, hi = max(0, i - 64), min(n - 3, i + 64)
        w = np.stack([a2[lo:hi], a2[lo + 1:hi + 1], a2[lo + 2:hi + 2]])
        err = np.abs(w - V[:, None]).max(0)
        tol = 0.3 * np.abs(V).max() + 1.0
        for j in np.nonzero(err < tol)[0]:
            k = lo + j
            if abs(k - i) >= 3 and np.abs(a2[k:k + 3]).max() > 3:
                out.append((start + i * 4 - gmem.BASE, start + k * 4 - gmem.BASE, tuple(float(x) for x in a2[i:i + 3]), tuple(float(x) for x in V), tuple(float(x) for x in a2[k:k + 3])))
print(len(out), 'pairs')
seen = set()
for o in out:
    if 0x6F000000 <= o[0] < 0x70000000 or o[0] >= 0xC5000000: continue
    key = (o[0] >> 12, o[1] - o[0])
    if key in seen: continue
    seen.add(key)
    print('pos@%08X vel@%08X (%+d)  pos %s  D/dt %s  vel %s' % (o[0], o[1], o[1] - o[0], tuple(round(x, 1) for x in o[2]), tuple(round(x, 1) for x in o[3]), tuple(round(x, 1) for x in o[4])))
