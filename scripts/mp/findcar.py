#!/usr/bin/env python3
"""Find the player's car: float triples (x,y,z) that stayed put while the car stood still (S0 -> S1) and moved when the throttle was held (S1 -> S2).
Run while a race is on and the car is stopped. Sends '1 RT 2.5' through the command file between S1 and S2."""
import sys, time, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import gmem
p = gmem.pid()
s0 = gmem.snapshot(p); time.sleep(3); s1 = gmem.snapshot(p)
open('/tmp/ao/pad.cmd', 'a').write('1 RT 2.5\n'); time.sleep(2.4)
s2 = gmem.snapshot(p)
print('snapshots', len(s0), len(s1), len(s2), file=sys.stderr)
hits = []
for start, b1 in s1.items():
    b0 = s0.get(start); b2 = s2.get(start)
    if b0 is None or b2 is None or not (len(b0) == len(b1) == len(b2)): continue
    n = len(b1) // 4
    a0 = np.frombuffer(b0, '>f4', n); a1 = np.frombuffer(b1, '>f4', n); a2 = np.frombuffer(b2, '>f4', n)
    with np.errstate(all='ignore'):
        st = (a0 == a1) & np.isfinite(a1) & (np.abs(a1) < 1e5)
        d = np.abs(a2 - a1)
        mv = np.isfinite(d) & (d > 0.3) & (d < 200)
        ok = st[:-2] & st[1:-1] & st[2:-0 or None][:len(st) - 2] if False else None
        # triple i, i+1, i+2: all finite and static, at least two of the three moved by S2
        t = np.zeros(n - 2, bool)
        cnt = mv[:-2].astype(int) + mv[1:-1] + mv[2:]
        stat = st[:-2] & st[1:-1] & st[2:]
        # a triple counts if the components are finite in S1 and at least two moved in S2; "static" is required only where it did not move
        fin = np.isfinite(a1[:-2]) & np.isfinite(a1[1:-1]) & np.isfinite(a1[2:]) & (np.abs(a1[:-2]) < 1e5) & (np.abs(a1[1:-1]) < 1e5) & (np.abs(a1[2:]) < 1e5)
        same = (a0[:-2] == a1[:-2]) & (a0[1:-1] == a1[1:-1]) & (a0[2:] == a1[2:])
        dist = np.sqrt(d[:-2] ** 2 + d[1:-1] ** 2 + d[2:] ** 2)
        t = fin & same & (cnt >= 2) & (dist > 6) & (dist < 120)
    for i in np.nonzero(t)[0]:
        hits.append((start + int(i) * 4 - gmem.BASE, tuple(float(x) for x in a1[i:i + 3]), tuple(float(x) for x in a2[i:i + 3]), float(dist[i])))
print(len(hits), 'candidate triples')
from collections import Counter
print(sorted(Counter(h[0] >> 20 for h in hits).items())[:40])
hits=[h for h in hits if not (0x6F000000 <= h[0] < 0x70000000)]
print(len(hits),'outside the stack')
open('/tmp/ao/hits.txt','w').write(''.join('%08X %.3f %.3f %.3f %.3f %.3f %.3f %.2f\n' % (h[0], *h[1], *h[2], h[3]) for h in hits))
for h in hits[:0]:
    print('%08X  S1 (%9.2f %9.2f %9.2f)  S2 (%9.2f %9.2f %9.2f)  moved %.1f' % (h[0], *h[1], *h[2], h[3]))
