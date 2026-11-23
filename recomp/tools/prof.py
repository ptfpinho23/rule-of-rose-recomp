#!/usr/bin/env python3
"""prof.py BINARY SAMPLES [TOP] — summarize a ROR_PROF sample file by function."""
import bisect, collections, os, struct, subprocess, sys
binary, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
nm = subprocess.run([os.path.expanduser('~/.local/opt/clang/bin/llvm-nm'), '-n', '--defined-only', binary],
                    capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in 'tTwW':
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]
data = open(samples, 'rb').read()
pcs = struct.unpack('<%dQ' % (len(data) // 8), data)
base = pcs[0] - dict((n, a) for a, n in syms)['rt_lookup']
pcs = [pc - base for pc in pcs[1:]]
c = collections.Counter()
for pc in pcs:
    i = bisect.bisect_right(addrs, pc) - 1
    name = syms[i][1] if 0 <= i and pc - addrs[i] < 0x100000 else '(outside: libc/kernel)'
    if name.startswith('f_00'):
        name = '(recompiled game code)'
    c[name] += 1
total = len(pcs)
print(f'{total} samples')
for name, n in c.most_common(top):
    print(f'{100.0 * n / total:6.2f}%  {name}')
