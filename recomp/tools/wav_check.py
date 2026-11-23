#!/usr/bin/env python3
"""wav_check.py FILE.wav [--png OUT.png] [--from SEC] [--to SEC]

Data checks for a 48 kHz stereo s16 dump (ROR_WAV) without listening to it:
  * per-5-second table: RMS, peak, DC offset per channel
  * clipping (samples at full scale), longest run of identical non-zero samples
  * discontinuity test at the 960-sample frame boundaries: the mean absolute
    first difference across a boundary divided by the mean elsewhere
    (1.0 = boundaries are not special)
  * spectrum summary: share of energy below 4 kHz / above 12 kHz and spectral
    flatness (near 1 = white noise, music and speech are far below)
  * optional spectrogram PNG (time left to right, 0-24 kHz bottom to top,
    waveform envelope strip at the bottom)
Pure Python (no numpy on this machine).
"""
import array
import cmath
import math
import struct
import sys
import wave
import zlib

FRAME = 960


def fft(x):
    n = len(x)
    if n == 1:
        return x
    ev, od = fft(x[0::2]), fft(x[1::2])
    out = [0] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * od[k]
        out[k] = ev[k] + t
        out[k + n // 2] = ev[k] - t
    return out


def png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    args = sys.argv[1:]
    opt = {}
    for k in ('--png', '--from', '--to'):
        if k in args:
            i = args.index(k); opt[k] = args[i + 1]; del args[i:i + 2]
    w = wave.open(args[0], 'rb')
    assert w.getnchannels() == 2 and w.getsampwidth() == 2, 'expect stereo s16'
    rate = w.getframerate()
    a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
    n0 = int(float(opt.get('--from', 0)) * rate)
    n1 = min(len(a) // 2, int(float(opt['--to']) * rate)) if '--to' in opt else len(a) // 2
    L, R = a[2 * n0:2 * n1:2], a[2 * n0 + 1:2 * n1:2]
    n = len(L)
    print(f'{args[0]}: {rate} Hz, {n / rate:.1f} s analysed (from {n0 / rate:.1f} s)')

    print('  time     rmsL   rmsR  peakL  peakR    dcL    dcR')
    step = 5 * rate
    for s in range(0, n, step):
        l, r = L[s:s + step], R[s:s + step]
        m = len(l)
        dl, dr = sum(l) / m, sum(r) / m
        rl = math.sqrt(sum(v * v for v in l) / m); rr = math.sqrt(sum(v * v for v in r) / m)
        print(f'  {(n0 + s) / rate:5.0f}s {rl:7.0f}{rr:7.0f}{max(map(abs, l)):7d}{max(map(abs, r)):7d}{dl:7.1f}{dr:7.1f}')

    clip = sum(1 for v in L if v >= 32767 or v <= -32768) + sum(1 for v in R if v >= 32767 or v <= -32768)
    run = best = 0
    for i in range(1, n):
        if L[i] == L[i - 1] and L[i] != 0:
            run += 1; best = max(best, run)
        else:
            run = 0
    nz = sum(1 for v in L if v) + sum(1 for v in R if v)
    print(f'clipped samples: {clip}; longest run of identical non-zero samples (L): {best + 1}; non-zero: {100 * nz / (2 * n):.1f}%')
    dc_l, dc_r = sum(L) / n, sum(R) / n
    print(f'overall DC offset: L {dc_l:.2f}  R {dc_r:.2f}')

    # frame-boundary discontinuities: for each phase p in 0..959 the mean |x[i]-x[i-1]| over i = p (mod 960)
    for name, ch in (('L', L), ('R', R)):
        acc = [0.0] * FRAME; cnt = [0] * FRAME
        for i in range(1, n):
            d = ch[i] - ch[i - 1]
            p = (n0 + i) % FRAME
            acc[p] += abs(d); cnt[p] += 1
        means = [acc[p] / max(cnt[p], 1) for p in range(FRAME)]
        avg = sum(means) / FRAME
        if avg == 0:
            print(f'frame-boundary test {name}: silent'); continue
        worst = max(range(FRAME), key=lambda p: means[p])
        sd = math.sqrt(sum((m - avg) ** 2 for m in means) / FRAME)
        print(f'frame-boundary test {name}: |diff| at phase 0 = {means[0] / avg:.3f} x average; '
              f'largest phase {worst} = {means[worst] / avg:.3f} x; spread over phases {sd / avg:.3f}')

    # spectrum
    N = 1024
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / N) for i in range(N)]
    hop = max(N, (n // 900) // N * N or N)
    cols = []
    tot = [0.0] * (N // 2)
    for s in range(0, n - N, hop):
        seg = [(L[s + i] + R[s + i]) * 0.5 * win[i] for i in range(N)]
        sp = fft(seg)
        mag = [abs(sp[k]) for k in range(N // 2)]
        cols.append(mag)
        for k in range(N // 2):
            tot[k] += mag[k] * mag[k]
    e = sum(tot) or 1.0
    lo = sum(tot[:int(4000 * N / rate)]) / e
    hi = sum(tot[int(12000 * N / rate):]) / e
    nzb = [t for t in tot[1:] if t > 0]
    flat = math.exp(sum(math.log(t) for t in nzb) / len(nzb)) / (sum(nzb) / len(nzb)) if nzb else 0
    print(f'spectrum: {100 * lo:.1f}% of energy below 4 kHz, {100 * hi:.2f}% above 12 kHz, flatness {flat:.4f}')

    if '--png' in opt and cols:
        H = 256; strip = 64
        rows = [[0] * len(cols) for _ in range(H + strip)]
        for x, mag in enumerate(cols):
            for y in range(H):
                m = max(mag[2 * y], mag[2 * y + 1])
                v = 20 * math.log10(m + 1e-9) - 20          # dB, hann-windowed 1024-point
                rows[H - 1 - y][x] = max(0, min(255, int((v - 10) * 255 / 90)))
            s = x * hop
            pk = max(abs(v) for v in L[s:s + hop]) if s < n else 0
            hgt = int(pk * (strip - 1) / 32768)
            for y in range(strip):
                rows[H + strip - 1 - y][x] = 255 if y <= hgt else 40
        png(opt['--png'], len(cols), H + strip, rows)
        print(f'wrote {opt["--png"]} ({len(cols)}x{H + strip}, {hop / rate * 1000:.0f} ms per column)')


if __name__ == '__main__':
    main()
