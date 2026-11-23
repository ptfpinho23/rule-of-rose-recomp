"""cli.py — command-line interface to rorcore.

The C++ launcher shells out to `python3 -m rorcore <cmd>` to read the disc,
so the heavy ISO/RTPK parsing stays in one well-tested place.

Commands:
    info <iso>          print game metadata as JSON
    list <iso> [path]   list a disc directory as JSON
    files <iso> <path>  list entries in an RTPK on the disc as JSON
    extract <iso> <discpath> <dest>   extract a file to disk
"""
from __future__ import annotations

import argparse
import json
import sys

from .iso9660 import IsoImage
from .rpk import RpkArchive
from .game import load_game_info


def _emit(obj) -> None:
    json.dump(obj, sys.stdout, indent=2)
    sys.stdout.write("\n")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="rorcore", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("info", help="print game metadata as JSON")
    pi.add_argument("iso")

    pl = sub.add_parser("list", help="list a disc directory as JSON")
    pl.add_argument("iso")
    pl.add_argument("path", nargs="?", default="/")

    pf = sub.add_parser("files", help="list entries in an RTPK as JSON")
    pf.add_argument("iso")
    pf.add_argument("path", help="disc path to the .RPK")

    pe = sub.add_parser("extract", help="extract a disc file to disk")
    pe.add_argument("iso")
    pe.add_argument("discpath")
    pe.add_argument("dest")

    a = p.parse_args(argv)

    if a.cmd == "info":
        with IsoImage(a.iso) as iso:
            _emit(load_game_info(iso).to_dict())
        return 0

    if a.cmd == "list":
        with IsoImage(a.iso) as iso:
            entries = [
                {
                    "name": e.name,
                    "is_dir": e.is_dir,
                    "size": e.size,
                }
                for e in iso.list(a.path)
            ]
            _emit({"path": a.path, "entries": entries})
        return 0

    if a.cmd == "files":
        with IsoImage(a.iso) as iso:
            data = iso.read(a.path)
        try:
            arc = RpkArchive(data)
        except ValueError as e:
            _emit({"path": a.path, "error": str(e), "entries": []})
            return 1
        _emit(
            {
                "path": a.path,
                "count": len(arc),
                "entries": [
                    {"path": f.path, "name": f.name, "size": f.size}
                    for f in arc.files
                ],
            }
        )
        return 0

    if a.cmd == "extract":
        with IsoImage(a.iso) as iso:
            n = iso.extract(a.discpath, a.dest)
        print(f"extracted {n} bytes -> {a.dest}", file=sys.stderr)
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
