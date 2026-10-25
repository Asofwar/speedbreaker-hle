#!/usr/bin/env python3
"""usage: dumpobj.py <guest addr> [before] [after]: words around an address as hex and float of the running game"""
import sys, struct, os
sys.path.insert(0, os.path.dirname(__file__))
import gmem
p = gmem.pid(); mem = open('/proc/%d/mem' % p, 'rb', 0)
a = int(sys.argv[1], 16); b = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100; n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
mem.seek(gmem.BASE + a - b); d = mem.read(b + n)
for i in range(0, len(d), 16):
    w = struct.unpack('>4I', d[i:i + 16]); f = struct.unpack('>4f', d[i:i + 16])
    print('%08X %+5d  %08X %08X %08X %08X  %s' % (a - b + i, i - b, *w, ' '.join('%9.3f' % x if abs(x) < 1e6 and (abs(x) > 1e-6 or x == 0) else '        .' for x in f)))
