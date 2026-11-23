"""iso9660.py — minimal ISO9660 reader for PlayStation discs.

PlayStation discs (PS1/PS2) are ISO9660 images with a Sony twist: directory
records carry their extent locations and sizes in *little-endian* as well as
the standard big-endian fields, and the PVD's volume-id / root record use the
LE values. This reader follows the LE convention, which is what the Rule of
Rose disc (SLES-54218) uses.

Only what the launcher needs is implemented: locate the root directory from the
primary volume descriptor, walk directories, and stream a file by path.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from typing import Iterator, Optional, Tuple

SECTOR = 2048
PVD_LBA = 16


@dataclass(frozen=True)
class IsoEntry:
    name: str
    is_dir: bool
    lba: int
    size: int
    flags: int

    @property
    def path(self) -> str:
        return self.name


class IsoImage:
    """Read access to an ISO9660 image file (PlayStation LE convention)."""

    def __init__(self, path: str):
        self.path = path
        self._f = open(path, "rb")
        self._size = self._f.seek(0, 2)
        self._root: Optional[Tuple[int, int]] = None

    # -- low-level -------------------------------------------------------
    def _read(self, off: int, n: int) -> bytes:
        self._f.seek(off)
        return self._f.read(n)

    def read_lba(self, lba: int, n: int) -> bytes:
        return self._read(lba * SECTOR, n)

    def __enter__(self) -> "IsoImage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._f.close()

    # -- volume / root ---------------------------------------------------
    def _find_root(self) -> Tuple[int, int]:
        if self._root is not None:
            return self._root
        pvd = self.read_lba(PVD_LBA, SECTOR)
        # Standard ISO9660 PVD places the root directory record at offset 156.
        rec = pvd[156:156 + 34]
        lba = struct.unpack("<I", rec[2:6])[0]
        size = struct.unpack("<I", rec[10:14])[0]
        self._root = (lba, size)
        return self._root

    @property
    def volume_id(self) -> str:
        pvd = self.read_lba(PVD_LBA, SECTOR)
        return pvd[40:72].rstrip(b"\x00 ").decode("ascii", "replace")

    # -- directory parsing ----------------------------------------------
    def parse_dir(self, lba: int, size: int) -> list[IsoEntry]:
        data = self.read_lba(lba, size)
        entries: list[IsoEntry] = []
        off = 0
        while off < len(data):
            rec_len = data[off]
            if rec_len == 0:
                break
            if 34 <= rec_len < 512 and off + rec_len <= len(data):
                lba_v = struct.unpack("<I", data[off + 2:off + 6])[0]
                size_v = struct.unpack("<I", data[off + 10:off + 14])[0]
                flags = data[off + 25]
                fn_len = data[off + 32]
                if 1 <= fn_len <= 30:
                    raw = data[off + 33:off + 33 + fn_len]
                    name = raw.split(b";")[0].rstrip(b"\x00")
                    if name and name not in (b"\x00", b"\x01"):
                        try:
                            name_s = name.decode("ascii")
                        except UnicodeDecodeError:
                            name_s = name.decode("latin-1")
                        entries.append(
                            IsoEntry(name_s, bool(flags & 0x02), lba_v, size_v, flags)
                        )
                off += rec_len
            else:
                off += 1
        return entries

    # -- navigation ------------------------------------------------------
    def list(self, path: str = "/") -> list[IsoEntry]:
        """List the directory at `path` (relative to ISO root, '/'-separated)."""
        lba, size = self._resolve_dir(path)
        return self.parse_dir(lba, size)

    def walk(self, max_depth: int = 3) -> Iterator[Tuple[str, IsoEntry]]:
        """Yield (path, entry) for every node up to max_depth."""
        lba, size = self._find_root()
        yield from self._walk(lba, size, "", max_depth, 0)

    def _walk(self, lba, size, prefix, max_depth, depth):
        for e in self.parse_dir(lba, size):
            if e.name in (".", ".."):
                continue
            path = prefix + e.name
            yield path, e
            if e.is_dir and depth < max_depth:
                yield from self._walk(e.lba, e.size, path + "/", max_depth, depth + 1)

    def _resolve_dir(self, path: str) -> Tuple[int, int]:
        parts = [p for p in path.split("/") if p and p != "."]
        lba, size = self._find_root()
        for part in parts:
            found = None
            for e in self.parse_dir(lba, size):
                if e.name.upper() == part.upper():
                    found = e
                    break
            if found is None or not found.is_dir:
                raise FileNotFoundError(f"no directory '{part}' in '{path}'")
            lba, size = found.lba, found.size
        return lba, size

    def exists(self, path: str) -> bool:
        try:
            self.stat(path)
            return True
        except FileNotFoundError:
            return False

    def stat(self, path: str) -> IsoEntry:
        """Return the entry for a file path relative to the ISO root."""
        parts = [p for p in path.split("/") if p and p != "."]
        if not parts:
            raise FileNotFoundError("/")
        lba, size = self._find_root()
        entry: Optional[IsoEntry] = None
        for part in parts:
            found = None
            for e in self.parse_dir(lba, size):
                if e.name.upper() == part.upper():
                    found = e
                    break
            if found is None:
                raise FileNotFoundError(path)
            entry = found
            if not found.is_dir and part != parts[-1]:
                raise FileNotFoundError(f"{part} is not a directory in {path}")
            lba, size = found.lba, found.size
        assert entry is not None
        return entry

    def read(self, path: str) -> bytes:
        e = self.stat(path)
        return self.read_lba(e.lba, e.size)

    def extract(self, path: str, dest: str) -> int:
        e = self.stat(path)
        data = self.read_lba(e.lba, e.size)
        parent = os.path.dirname(dest)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(dest, "wb") as out:
            out.write(data)
        return len(data)
