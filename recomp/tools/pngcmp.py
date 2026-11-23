#!/usr/bin/env python3
"""pngcmp.py A.png B.png [...pairs] — pixel comparison of 8-bit RGB PNGs of
equal size (as written by the game's --shots): differing pixels, how many
differ by more than 1 and 8 in a channel, and the largest difference."""
import sys, zlib, struct
def load(p):
    d = open(p, 'rb').read(); pos = 8; idat = b''; w = h = 0
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        if t == b'IHDR': w, h = struct.unpack('>II', d[pos + 8:pos + 16])
        if t == b'IDAT': idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat); row = w * 3 + 1
    assert all(raw[y * row] == 0 for y in range(h)), 'filtered PNG rows not supported'
    return w, h, b''.join(raw[y * row + 1:(y + 1) * row] for y in range(h))
a = sys.argv[1:]
for i in range(0, len(a) - 1, 2):
    wa, ha, pa = load(a[i]); wb, hb, pb = load(a[i + 1])
    if (wa, ha) != (wb, hb): print(f'{a[i]} vs {a[i+1]}: sizes differ {wa}x{ha} / {wb}x{hb}'); continue
    n = n1 = n8 = mx = 0
    if pa != pb:
        for k in range(0, len(pa), 3):
            if pa[k:k + 3] != pb[k:k + 3]:
                m = max(abs(pa[k] - pb[k]), abs(pa[k + 1] - pb[k + 1]), abs(pa[k + 2] - pb[k + 2]))
                n += 1; n1 += m > 1; n8 += m > 8; mx = max(mx, m)
    t = wa * ha
    print(f'{a[i].split("/")[-1]}: {n} of {t} pixels differ ({100*n/t:.3f}%), >1: {100*n1/t:.3f}%, >8: {100*n8/t:.4f}%, max {mx}')
