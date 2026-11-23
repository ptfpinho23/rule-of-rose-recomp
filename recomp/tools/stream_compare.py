#!/usr/bin/env python3
"""stream_compare.py OUT.wav CH0.adp [CH1.adp] [--iso]

Compares the mixer's output (ROR_WAV dump) with an independent offline decode
of the raw ADPCM the game fed to one stream (ROR_SNDDUMP=prefix writes
prefix_slotN_openK_chC.adp). A stream played at pitch 0x1000 must appear in
the WAV as the decoded samples times a constant gain; the script locates it
(sign-pattern search), fits the gain and reports correlation and residual
per 5 seconds for each channel. With --iso it also looks the fed bytes up in
the disc image to confirm they are disc data (and prints where).
"""
import array
import math
import mmap
import sys
import wave

F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)


def decode(d):
    out = array.array('h')
    s1 = s2 = 0
    for o in range(0, len(d) - 15, 16):
        sh, fl = d[o] & 15, d[o] >> 4
        if fl > 4:
            fl = 4
        if sh > 12:
            sh = 9
        for i in range(28):
            n = (d[o + 2 + (i >> 1)] >> ((i & 1) * 4)) & 15
            if n >= 8:
                n -= 16
            x = ((n << 12) >> sh) + ((s1 * F0[fl] + s2 * F1[fl] + 32) >> 6)
            x = max(-32768, min(32767, x))
            out.append(x)
            s2, s1 = s1, x
    return out


def main():
    args = [a for a in sys.argv[1:] if a != '--iso']
    w = wave.open(args[0], 'rb')
    a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
    chans = (a[0::2], a[1::2])
    for ci, path in enumerate(args[1:3]):
        raw = open(path, 'rb').read()
        dec = decode(raw)
        out = chans[ci]
        outsign = bytes(1 if v > 0 else 0 for v in out)
        off = None
        for start in range(28 * 2000, len(dec) - 400, 28 * 500):
            seg = dec[start:start + 300]
            if min(abs(v) for v in seg) < 1 or sum(1 for v in seg if abs(v) < 300) > 60:
                continue
            key = bytes(1 if v > 0 else 0 for v in seg)
            p = outsign.find(key)
            if p >= 0 and outsign.find(key, p + 1) < 0:
                off = p - start; break
        print(f'{path}: {len(raw)} bytes = {len(dec) / 48000:.2f} s decoded offline')
        if off is None:
            print('  NOT FOUND in the WAV'); continue
        print(f'  found in WAV channel {ci} at sample offset {off} ({off / 48000:.3f} s, frame {off / 960:.2f})')
        print('    t(s)   gain   corr   residual/signal')
        worst = 1.0
        for s in range(0, len(dec), 5 * 48000):
            e = min(len(dec), s + 5 * 48000, len(out) - off)
            if e - s < 4800:
                break
            x = dec[s:e]; y = out[off + s:off + e]
            sxx = sum(v * v for v in x); syy = sum(v * v for v in y); sxy = sum(p * q for p, q in zip(x, y))
            if not sxx or not syy:
                print(f'  {s / 48000:6.0f}   (silent)'); continue
            g = sxy / sxx
            res = sum((q - g * p) ** 2 for p, q in zip(x, y))
            corr = sxy / math.sqrt(sxx * syy)
            worst = min(worst, corr)
            print(f'  {s / 48000:6.0f} {g:6.4f} {corr:7.5f} {math.sqrt(res / syy):8.5f}')
        print(f'  lowest correlation: {worst:.5f}')
        if '--iso' in sys.argv:
            import glob
            iso = glob.glob('*.iso')[0]
            f = open(iso, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            for blk in (1000, 20000, 60000):
                key = raw[blk * 16 + 2:blk * 16 + 16]          # 14 data bytes (the EE rewrites the flag byte)
                p = m.find(key)
                print(f'  fed block {blk}: ' + (f'found on disc at byte 0x{p - 2:x} (sector {(p - 2) // 2048}, +{(p - 2) % 2048})' if p >= 0 else 'NOT on disc'))


if __name__ == '__main__':
    main()
