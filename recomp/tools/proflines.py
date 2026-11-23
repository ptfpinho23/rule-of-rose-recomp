#!/usr/bin/env python3
"""proflines.py BINARY SAMPLES [TOP] [FUNC] — like prof.py, but by source line
(innermost inlined frame), optionally only samples inside symbol FUNC."""
import collections, os, struct, subprocess, sys
binary, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
only = sys.argv[4] if len(sys.argv) > 4 else None
bindir = os.path.expanduser('~/.local/opt/clang/bin/')
nm = subprocess.run([bindir + 'llvm-nm', '-n', '--defined-only', binary], capture_output=True, text=True).stdout
base_sym = [int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == 'rt_lookup'][0]
data = open(samples, 'rb').read()
pcs = struct.unpack('<%dQ' % (len(data) // 8), data)
base = pcs[0] - base_sym
cnt = collections.Counter(pc - base for pc in pcs[1:])
addrs = sorted(cnt)
out = subprocess.run([bindir + 'llvm-symbolizer', '--obj=' + binary, '--output-style=GNU', '--inlines', '--functions=linkage'],
                     input='\n'.join(hex(a) for a in addrs), capture_output=True, text=True).stdout
lines = collections.Counter(); total = 0
blocks = out.strip().split('\n\n') if '\n\n' in out else None
# GNU style without blank separators: parse sequentially, each address yields (func, file:line) pairs, inlined first
it = iter(out.splitlines())
res = []
cur = []
for l in it:
    if l == '':
        res.append(cur); cur = []
    else:
        cur.append(l)
if cur: res.append(cur)
if len(res) != len(addrs):  # fallback: one address at a time
    res = []
    for a in addrs:
        o = subprocess.run([bindir + 'llvm-symbolizer', '--obj=' + binary, '--output-style=GNU', '--inlines', hex(a)], capture_output=True, text=True).stdout.strip().splitlines()
        res.append(o)
for a, fr in zip(addrs, res):
    funcs = fr[0::2]; locs = fr[1::2]
    outer = funcs[-1] if funcs else '?'
    if only and only not in funcs: continue
    loc = locs[0].split('/')[-1] if locs else '?'
    lines[(loc, funcs[0] if funcs else '?')] += cnt[a]; total += cnt[a]
print(f'{total} samples')
for (loc, fn), n in lines.most_common(top):
    print(f'{100.0 * n / total:6.2f}%  {loc:28s} {fn}')
