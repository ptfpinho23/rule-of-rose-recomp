"""game.py — game metadata extracted from the Rule of Rose disc."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional

from .iso9660 import IsoImage

_LANGS = {
    "ENG": "English",
    "FRE": "French",
    "GER": "German",
    "ITA": "Italian",
    "SPA": "Spanish",
}


@dataclass
class GameInfo:
    """Metadata about a Rule of Rose disc, for the launcher to display."""

    present: bool = False
    serial: str = ""
    version: str = ""
    vmode: str = ""
    country: str = ""
    language: str = ""
    languages: list[str] = field(default_factory=list)
    main_exe: str = ""
    main_exe_size: int = 0
    has_modules: bool = False
    movie_count: int = 0
    world_count: int = 0
    iso_path: str = ""

    @property
    def display_version(self) -> str:
        # PS2 SYSTEM.INI encodes the version as e.g. "102" meaning 1.02.
        v = self.version.strip()
        if v.isdigit() and len(v) >= 2:
            return v[0] + "." + v[1:]
        return v or "1.00"

    def to_dict(self) -> dict:
        return {
            "present": self.present,
            "serial": self.serial,
            "version": self.display_version,
            "vmode": self.vmode,
            "country": self.country,
            "language": self.language,
            "languages": self.languages,
            "main_exe": self.main_exe,
            "main_exe_size": self.main_exe_size,
            "has_modules": self.has_modules,
            "movie_count": self.movie_count,
            "world_count": self.world_count,
            "iso_path": self.iso_path,
        }


def _parse_system_ini(text: str) -> dict[str, str]:
    """Parse the [SYSTEM] / [APPLICATION] sections of SYSTEM.INI."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";", "[")):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip().upper()] = v.strip()
    return out


def load_game_info(iso: IsoImage) -> GameInfo:
    """Read SYSTEM.INI and the disc layout to build a GameInfo."""
    info = GameInfo(iso_path=iso.path)
    info.present = True

    # SYSTEM.INI
    try:
        ini = iso.read("/SYSTEM.INI").decode("latin-1", "replace")
        kv = _parse_system_ini(ini)
        info.serial = kv.get("NUMBER", "")
        info.version = kv.get("VERSION", "")
        info.vmode = kv.get("VMODE", "")
        info.country = kv.get("COUNTRY", "")
        info.language = kv.get("LANGUAGE", "")
    except FileNotFoundError:
        pass

    # Main executable
    try:
        e = iso.stat("/SLES_542.18")
        info.main_exe = e.name
        info.main_exe_size = e.size
        # serial from the filename if the INI didn't have it
        if not info.serial:
            info.serial = e.name.replace("_", "-")
    except FileNotFoundError:
        pass

    # Languages: which EVT_XXX dirs exist
    langs = []
    try:
        for entry in iso.list("/DATA"):
            m = re.match(r"EVT_([A-Z]{3})$", entry.name)
            if m:
                code = m.group(1)
                if code in _LANGS:
                    langs.append(_LANGS[code])
    except FileNotFoundError:
        pass
    info.languages = langs

    # Module presence
    try:
        iso.list("/MODULES")
        info.has_modules = True
    except FileNotFoundError:
        pass

    # Counts (movie + worlds)
    try:
        info.movie_count = len(iso.list("/MOVIE"))
    except FileNotFoundError:
        pass
    try:
        worlds = [e for e in iso.list("/DATA/SEQ_ENG") if e.is_dir]
        info.world_count = len(worlds)
    except FileNotFoundError:
        pass

    return info
