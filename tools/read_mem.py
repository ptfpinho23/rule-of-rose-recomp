#!/usr/bin/env python3
"""Read the R struct + debug markers from a running ror_native process's /proc/PID/mem."""
import struct, sys

pid = int(sys.argv[1])

def rd(off, size=8):
    with open(f"/proc/{pid}/mem", "rb") as f:
        f.seek(off)
        return f.read(size)

print("GPR dump (R struct at 0x500000):")
names = ["zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
         "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"]
for i, nm in enumerate(names):
    v = struct.unpack("<Q", rd(0x500000 + i*8))[0]
    if v:
        print(f"  ${nm:4s}[{i:2d}] = 0x{v:x}")

print("Markers:")
for off in (0x297F00, 0x297F01, 0x297F10, 0x297F20, 0x297F21, 0x297F22, 0x297F23, 0x297F24):
    v = struct.unpack("<B", rd(off, 1))[0]
    print(f"  0x{off:x} = 0x{v:02x}")

print("FPU slots 48..86 (nonzero):")
for i in range(48, 87):
    v = struct.unpack("<Q", rd(0x500000 + i*8))[0]
    if v:
        print(f"  f[{i-48}] = 0x{v:x}")
