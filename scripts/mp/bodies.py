#!/usr/bin/env python3
"""List rigid-body-like blocks: three orthonormal 16-byte rows (w = 0) followed by a position row (w = 1.0), then a velocity row (w = 0)."""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import gmem
p = gmem.pid(); s = gmem.snapshot(p)
res = []
for start, b in s.items():
    n = len(b) // 4
    a = np.frombuffer(b, '>f4', n)
    if n < 24: continue
    m = n - 20
    with np.errstate(all='ignore'):
        # rows at i, i+4, i+8 (xyz at +0..2, w at +3), position row at i+12, velocity at i+16
        ok = (a[3:m + 3] == 0) & (a[7:m + 7] == 0) & (a[11:m + 11] == 0) & (a[15:m + 15] == 1.0)
        r0 = np.sqrt(a[0:m] ** 2 + a[1:m + 1] ** 2 + a[2:m + 2] ** 2)
        r1 = np.sqrt(a[4:m + 4] ** 2 + a[5:m + 5] ** 2 + a[6:m + 6] ** 2)
        r2 = np.sqrt(a[8:m + 8] ** 2 + a[9:m + 9] ** 2 + a[10:m + 10] ** 2)
        ok &= (np.abs(r0 - 1) < 0.01) & (np.abs(r1 - 1) < 0.01) & (np.abs(r2 - 1) < 0.01)
        ok &= (a[19:m + 19] == 0)
    for i in np.nonzero(ok)[0]:
        pos = a[12 + i:15 + i]; vel = a[16 + i:19 + i]
        res.append((start + (int(i) + 12) * 4 - gmem.BASE, tuple(float(x) for x in pos), tuple(float(x) for x in vel)))
for r in res:
    print('%08X pos (%9.2f %9.2f %9.2f) vel (%7.2f %7.2f %7.2f)' % (r[0], *r[1], *r[2]))
print(len(res), 'blocks')
