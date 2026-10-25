"""Helpers: read the running game's guest memory (host base 0x100000000) through /proc/<pid>/mem, only the pages that are resident."""
import os, struct, subprocess
BASE = 0x100000000
PAGE = os.sysconf("SC_PAGE_SIZE")
def pid():
    return int(subprocess.check_output(['pgrep', '-x', 'SpeedBreaker']).split()[0])
def resident_ranges(p):
    """(host start, length) of runs of resident pages inside the guest 4 GB."""
    runs = []
    pm = open('/proc/%d/pagemap' % p, 'rb')
    for line in open('/proc/%d/maps' % p):
        a, perm = line.split()[:2]
        if not perm.startswith('rw'): continue
        s, e = [int(x, 16) for x in a.split('-')]
        if s < BASE or e > BASE + (1 << 32): continue
        pm.seek(s // PAGE * 8)
        data = pm.read((e - s) // PAGE * 8)
        n = len(data) // 8
        ents = struct.unpack('<%dQ' % n, data)
        run = None
        for i, en in enumerate(ents):
            present = (en >> 63) & 1
            if present and run is None: run = i
            if not present and run is not None:
                runs.append((s + run * PAGE, (i - run) * PAGE)); run = None
        if run is not None: runs.append((s + run * PAGE, (n - run) * PAGE))
    return runs
def snapshot(p):
    """dict host start -> bytes for every resident run"""
    mem = open('/proc/%d/mem' % p, 'rb', 0)
    out = {}
    for s, l in resident_ranges(p):
        try:
            mem.seek(s); out[s] = mem.read(l)
        except OSError:
            pass
    return out
if __name__ == '__main__':
    p = pid(); r = resident_ranges(p)
    print(len(r), 'runs', sum(l for _, l in r) / 1048576, 'MB resident')
