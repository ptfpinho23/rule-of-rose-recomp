#!/usr/bin/env python3
"""isoextract.py ISO NAME OUT — copy one file of the root directory of an
ISO 9660 image (2048-byte sectors) to OUT. NAME is matched without the
";1" version suffix, e.g. SLES_542.18."""
import struct, sys

def extract(iso, name, out):
    with open(iso, 'rb') as f:
        f.seek(16 * 2048); pvd = f.read(2048)
        if pvd[1:6] != b'CD001': raise SystemExit('isoextract: %s is not an ISO 9660 image' % iso)
        lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
        f.seek(lba * 2048); d = f.read(size); o = 0
        while o < len(d):
            n = d[o]
            if n == 0: o = (o // 2048 + 1) * 2048; continue
            flen = d[o + 32]; fname = d[o + 33:o + 33 + flen].decode('latin1').split(';')[0]
            if fname.upper() == name.upper() and not d[o + 25] & 2:
                flba, fsize = struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0]
                f.seek(flba * 2048); data = f.read(fsize)
                if len(data) != fsize: raise SystemExit('isoextract: %s is truncated' % iso)
                open(out, 'wb').write(data); return
            o += n
    raise SystemExit('isoextract: %s has no %s in its root directory (not that edition of Rule of Rose?)' % (iso, name))

if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2], sys.argv[3])
