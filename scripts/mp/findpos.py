#!/usr/bin/env python3
"""The player's car position is in a static triple (guest 0x82A2DBF0 in a Quick Play race). Find every other place holding the same three floats,
drive, and report which of them follow it."""
import sys, struct, time, os
sys.path.insert(0, os.path.dirname(__file__))
import gmem
p = gmem.pid()
SRC = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x82A2DBF0
mem = open('/proc/%d/mem' % p, 'rb', 0)
def rd(a, n):
    mem.seek(gmem.BASE + a); return mem.read(n)
def triple(a): return struct.unpack('>3f', rd(a, 12))
t0 = triple(SRC); print('source', hex(SRC), t0)
pat = rd(SRC, 12)
s = gmem.snapshot(p)
hits = []
for start, b in s.items():
    i = b.find(pat)
    while i >= 0:
        if i % 4 == 0: hits.append(start + i - gmem.BASE)
        i = b.find(pat, i + 4)
print(len(hits), 'places hold it:', ' '.join('%08X' % h for h in hits[:60]))
open('/tmp/ao/pad.cmd', 'a').write('1 RT 2.0\n'); time.sleep(2.2)
t1 = triple(SRC); print('source now', t1)
follow = [h for h in hits if triple(h) == t1]
print('still equal to the source after driving:', ' '.join('%08X' % h for h in follow))
moved = [h for h in hits if triple(h) != t0]
print('changed:', len(moved))
