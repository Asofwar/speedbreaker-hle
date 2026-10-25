#!/usr/bin/env python3
"""usage: ptrs.py <lo> <hi>: every aligned word in the resident guest memory whose value is in [lo, hi), with its address"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import gmem
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
p = gmem.pid(); s = gmem.snapshot(p)
for start, b in sorted(s.items()):
    a = np.frombuffer(b, '>u4', len(b) // 4)
    for i in np.nonzero((a >= lo) & (a < hi))[0]:
        print('%08X -> %08X' % (start + int(i) * 4 - gmem.BASE, a[i]))
