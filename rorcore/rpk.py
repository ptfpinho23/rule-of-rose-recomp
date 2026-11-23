"""rpk.py — Rule of Rose RTPK container parser.

RTPK (magic "RTPK") is the game's asset container: textures (TIM2), scene data,
camera files, and scene scripts packed together.

Structure (little-endian):
    0x00  magic "RTPK"
    0x04  u32  total container size
    0x10  u32  file-table byte length
    0x20  FILE TABLE — a region of `file-table byte length` bytes containing,
          in order: the (data_offset, data_size) u32 pairs for each entry,
          followed by the null-terminated entry path strings. The numeric and
          textual parts are contiguous; the boundary is not separately marked,
          so we recover entries by scanning the region for "a/b" path strings
          and pairing them in order with the leading (offset, size) values.

Payloads are addressed by data_offset from the start of the file.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

_MAGIC = b"RTPK"
_TABLE_AT = 0x20


@dataclass(frozen=True)
class RpkFile:
    path: str
    offset: int
    size: int

    @property
    def name(self) -> str:
        return self.path.rsplit("/", 1)[-1]


def _is_pathlike(s: str) -> bool:
    if len(s) < 3 or len(s) > 60:
        return False
    if s[0] in "./\\":
        return False
    # must be a plausible filename: contains a dot-extension or a slash,
    # and only safe characters.
    if not all(32 <= ord(c) < 127 for c in s):
        return False
    if "." not in s and "/" not in s:
        return False
    return True


def _paths_in(d: bytes, start: int, end: int) -> list[tuple[int, str]]:
    """(byte_offset, path) for each path-like string fully inside [start, end)."""
    out: list[tuple[int, str]] = []
    i = start
    while i < end - 2:
        if 32 <= d[i] < 127:
            j = i
            while j < end and (32 <= d[j] < 127 or d[j] in (46, 47, 95)):
                j += 1
            s = d[i:j].decode("latin-1")
            if _is_pathlike(s):
                out.append((i, s))
            i = j if j > i else i + 1
        else:
            i += 1
    return out


class RpkArchive:
    """Parse and read entries from an RTPK container (raw bytes)."""

    def __init__(self, data: bytes):
        self._data = data
        self._files: list[RpkFile] = []
        self._by_path: dict[str, RpkFile] = {}
        self._parse()

    @classmethod
    def open(cls, path: str) -> "RpkArchive":
        with open(path, "rb") as f:
            return cls(f.read())

    def _parse(self) -> None:
        d = self._data
        if len(d) < _TABLE_AT + 16 or d[:4] != _MAGIC:
            raise ValueError("not an RTPK container (bad magic)")
        total = struct.unpack_from("<I", d, 0x04)[0]
        total = min(total, len(d)) if total else len(d)
        table_len = struct.unpack_from("<I", d, 0x10)[0]
        # The table region: the declared table length, but at least enough to
        # hold the header, and capped well inside the file (paths never appear
        # past the first ~64 KiB of a container).
        region = max(table_len, 0x40) if table_len else 0x100
        t_end = min(_TABLE_AT + region, total, 0x10000)

        # All plausible (offset, size) pairs anywhere in the table region.
        pairs: list[tuple[int, int]] = []
        o = _TABLE_AT
        while o + 8 <= t_end:
            off, size = struct.unpack_from("<II", d, o)
            if 0 < off < total and 0 < size < total:
                pairs.append((off, size))
            o += 8

        paths = _paths_in(d, _TABLE_AT, t_end)
        n = min(len(paths), len(pairs))
        for i in range(n):
            f = RpkFile(paths[i][1], pairs[i][0], pairs[i][1])
            self._add(f)

    def _add(self, f: RpkFile) -> None:
        if f.size > 0 and f.path and f.path not in self._by_path:
            self._files.append(f)
            self._by_path[f.path] = f

    # -- access ----------------------------------------------------------
    @property
    def files(self) -> list[RpkFile]:
        return list(self._files)

    def __len__(self) -> int:
        return len(self._files)

    def __iter__(self):
        return iter(self._files)

    def get(self, path: str) -> Optional[RpkFile]:
        return self._by_path.get(path)

    def read(self, path: str) -> Optional[bytes]:
        f = self._by_path.get(path)
        if f is None:
            return None
        return self._data[f.offset : f.offset + f.size]
