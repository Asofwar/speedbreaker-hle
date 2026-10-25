#!/bin/sh
# usage: dis.sh <guest addr hex> [bytes]  -- disassemble from the guest image dump
python3 - "$1" "${2:-256}" <<'PY' > /tmp/d.hex
import sys
a=int(sys.argv[1],16); n=int(sys.argv[2])
d=open('/home/dhika/.cache/NFS-MostWanted/tools/image.bin','rb').read()
o=a-0x82000000
print(' '.join('0x%02x'%b for b in d[o:o+n]))
PY
llvm-mc --disassemble --triple=powerpc64 /tmp/d.hex 2>&1 | grep -v "^\s*\.text" | awk -v b=$((0x$1)) '{printf "%X %s\n", b+(NR-1)*4, $0}'
