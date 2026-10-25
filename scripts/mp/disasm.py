#!/usr/bin/env python3
"""usage: dis.py <guest addr hex> [bytes]: disassemble the guest image dump (tools/image.bin), one 4-byte word at a time so an undecodable word (VMX128) never shifts the addresses."""
import sys, os, subprocess
a = int(sys.argv[1], 16) & ~3; n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 256
d = open(os.path.expanduser('~/.cache/NFS-MostWanted/tools/image.bin'), 'rb').read()
o = a - 0x82000000
words = [d[o + i:o + i + 4] for i in range(0, n, 4)]
src = '\n'.join(' '.join('0x%02x' % b for b in w) for w in words)
# one llvm-mc process per word keeps the output aligned
out = []
for w in words:
    r = subprocess.run(['llvm-mc', '--disassemble', '--triple=powerpc64'], input=' '.join('0x%02x' % b for b in w), capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith('.text')]
    out.append(lines[0] if lines else '.long 0x%08x' % int.from_bytes(w, 'big'))
for i, t in enumerate(out):
    print('%08X  %s' % (a + 4 * i, t))
