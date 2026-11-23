"""Minimal ELF32-LE reader for the PS2 game executable."""
import struct
from dataclasses import dataclass


@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int


@dataclass
class Segment:
    type: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int


class Elf:
    def __init__(self, path):
        self.data = d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 1, 'not ELF32-LE'
        (self.entry, phoff, shoff, _flags, _ehsz, phentsz, phnum, shentsz,
         shnum, shstrndx) = struct.unpack_from('<IIIIHHHHHH', d, 0x18)
        self.segments = []
        for i in range(phnum):
            t, off, va, _pa, fsz, msz, fl, _al = struct.unpack_from(
                '<IIIIIIII', d, phoff + i * phentsz)
            self.segments.append(Segment(t, off, va, fsz, msz, fl))
        raw = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsz)
               for i in range(shnum)]
        stroff = raw[shstrndx][4]
        self.sections = []
        for i, (nm, t, fl, addr, off, size, *_r) in enumerate(raw):
            end = d.index(b'\0', stroff + nm)
            self.sections.append(Section(i, d[stroff + nm:end].decode('latin1'),
                                         t, fl, addr, off, size))

    def loads(self):
        return [s for s in self.segments if s.type == 1]

    def read(self, vaddr, n):
        for s in self.loads():
            if s.vaddr <= vaddr and vaddr + n <= s.vaddr + s.filesz:
                o = s.offset + vaddr - s.vaddr
                return self.data[o:o + n]
        raise KeyError(hex(vaddr))

    def word(self, vaddr):
        return struct.unpack('<I', self.read(vaddr, 4))[0]
