#!/usr/bin/env python3
"""Walk the game's list of dynamic objects (head 0x8290BE18, links at obj+16; sub_825285C8 / sub_82528448) in the running game."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(__file__))
import gmem
p = gmem.pid(); m = open('/proc/%d/mem' % p, 'rb', 0)
def u32(a): m.seek(gmem.BASE + a); return struct.unpack('>I', m.read(4))[0]
def f3(a): m.seek(gmem.BASE + a); return struct.unpack('>3f', m.read(12))
node = u32(0x8290BE18); n = 0; end = 0x8290BE18 - 16 + 16
while node and n < 200 and node - 16 != 0x8290BE08:
    obj = node - 16
    vt = u32(obj); par = u32(obj + 8); st = u32(obj + 28)
    try:
        pos = f3(st + 64); vel = f3(st + 80)
    except Exception: pos = vel = None
    print('obj %08X vt %08X [+8] %08X state %08X pos %s vel %s' % (obj, vt, par, st, tuple(round(x, 1) for x in pos) if pos else None, tuple(round(x, 1) for x in vel) if vel else None))
    node = u32(obj + 16); n += 1
print(n, 'objects')
