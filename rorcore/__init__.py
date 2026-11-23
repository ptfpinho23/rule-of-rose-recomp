"""rorcore — Rule of Rose PC port core library.

Reads the PS2 disc (ISO9660), parses the RTPK asset containers, and exposes
game metadata for the launcher.
"""
from .iso9660 import IsoImage, IsoEntry
from .rpk import RpkArchive, RpkFile
from .game import GameInfo, load_game_info

__all__ = ["IsoImage", "IsoEntry", "RpkArchive", "RpkFile", "GameInfo", "load_game_info"]
