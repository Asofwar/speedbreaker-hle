#!/usr/bin/env python3
"""usage: localvel.py <pos addr> [span]: drive, read the area around a known position twice, list triples that equal (pos2-pos1)/dt (the velocity)"""
import sys, struct, time, os
sys.path.insert(0, os.path.dirname(__file__))
import gmem
p = gmem.pid(); mem = open('/proc/%d/mem' % p, 'rb', 0)
a = int(sys.argv[1], 16); span = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x800
def rd():
    mem.seek(gmem.BASE + a - span); return mem.read(2 * span), time.time()
open('/tmp/ao/pad.cmd', 'a').write('1 RT 6.0\n'); time.sleep(4.0)
d1, t1 = rd(); time.sleep(0.25); d2, t2 = rd()
dt = t2 - t1
P1 = struct.unpack('>3f', d1[span:span + 12]); P2 = struct.unpack('>3f', d2[span:span + 12])
V = [(q - o) / dt for o, q in zip(P1, P2)]
print('dt %.3f pos %s -> %s  D/dt %s |v| %.1f' % (dt, P1, P2, [round(x, 2) for x in V], sum(x * x for x in V) ** .5))
n = len(d2) // 4
w2 = struct.unpack('>%df' % n, d2)
for i in range(n - 2):
    t = w2[i:i + 3]
    if all(abs(x - v) < 0.25 * max(abs(v), 4) + 1 for x, v in zip(t, V)) and max(abs(x) for x in t) > 4:
        print('%08X (%+5d) %s' % (a - span + 4 * i, 4 * i - span, tuple(round(x, 2) for x in t)))
# also any word that equals the speed magnitude or a pos component delta, for reference
sp = sum(x * x for x in V) ** .5
for i in range(n):
    if abs(w2[i] - sp) < 0.04 * sp:
        print('speed-like %08X (%+5d) %.2f' % (a - span + 4 * i, 4 * i - span, w2[i]))
