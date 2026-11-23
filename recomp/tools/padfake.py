#!/usr/bin/env python3
# PS2 --script ("frame:activelowhex,...") -> ROR_PAD_FAKE in Xbox terms.
# argv[2] = "stick": D-pad directions become full left-stick tilt instead; "half": 60 % tilt
import sys
M = {0x0001:0x20,0x0002:0x40,0x0004:0x80,0x0008:0x10,0x0010:1,0x0020:8,0x0040:2,0x0080:4,0x0400:0x100,0x0800:0x200,0x1000:0x8000,0x2000:0x2000,0x4000:0x1000,0x8000:0x4000}
mode = sys.argv[2] if len(sys.argv) > 2 else ""
frm = int(sys.argv[3]) if len(sys.argv) > 3 else 0     # sticks only from this frame on
out = []
for ent in sys.argv[1].split(","):
    f, b = ent.split(":"); f = int(f); p = ~int(b, 16) & 0xFFFF
    x = 0; lx = ly = 0; lt = rt = 0
    amp = 32767 if mode == "stick" else int(32767 * 0.6)
    for k, v in M.items():
        if p & k:
            if mode and f >= frm and k in (0x10, 0x20, 0x40, 0x80):
                if k == 0x10: ly = amp
                if k == 0x40: ly = -amp
                if k == 0x20: lx = amp
                if k == 0x80: lx = -amp
            else: x |= v
    if p & 0x100: lt = 255
    if p & 0x200: rt = 255
    out.append("%d:%x:%d:%d:0:0:%d:%d" % (f, x, lx, ly, lt, rt))
print(",".join(out))
