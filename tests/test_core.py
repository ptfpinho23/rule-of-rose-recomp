"""Tests for rorcore. Run: python3 -m pytest tests/ -q   (or just: python3 tests/test_core.py)"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from rorcore import IsoImage, RpkArchive, load_game_info  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, "Rule of Rose (Europe) (En,Fr,De,Es,It).iso")

# A couple of small RTPK files, if present (extracted from the disc).
ROR_EXTRACT = "/tmp/ror-extract"


class TestIso(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(ISO):
            self.skipTest("disc ISO not present")
        self.iso = IsoImage(ISO)

    def tearDown(self):
        self.iso.close()

    def test_root_listing(self):
        names = {e.name for e in self.iso.list("/")}
        self.assertIn("DATA", names)
        self.assertIn("MOVIE", names)
        self.assertIn("MODULES", names)
        self.assertIn("SYSTEM.INI", names)

    def test_stat_main_exe(self):
        e = self.iso.stat("/SLES_542.18")
        self.assertEqual(e.size, 2456780)
        self.assertFalse(e.is_dir)

    def test_read_system_ini(self):
        text = self.iso.read("/SYSTEM.INI").decode("latin-1")
        self.assertIn("SLES-54218", text)

    def test_data_subdirs(self):
        names = {e.name for e in self.iso.list("/DATA") if e.is_dir}
        for want in ("BG", "CH", "SND", "EVT_ENG", "MNURPK"):
            self.assertIn(want, names)

    def test_walk_depth(self):
        nodes = list(self.iso.walk(max_depth=1))
        self.assertGreater(len(nodes), 0)
        # every node has a path and an entry
        for path, entry in nodes[:5]:
            self.assertTrue(path)
            self.assertTrue(entry.name)


class TestGameInfo(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(ISO):
            self.skipTest("disc ISO not present")
        self.iso = IsoImage(ISO)
        self.info = load_game_info(self.iso)

    def tearDown(self):
        self.iso.close()

    def test_serial_and_version(self):
        self.assertEqual(self.info.serial, "SLES-54218")
        self.assertEqual(self.info.display_version, "1.02")

    def test_languages(self):
        self.assertEqual(
            self.info.languages,
            ["English", "French", "German", "Italian", "Spanish"],
        )

    def test_counts(self):
        self.assertGreater(self.info.movie_count, 0)
        self.assertGreaterEqual(self.info.world_count, 13)
        self.assertTrue(self.info.has_modules)

    def test_to_dict(self):
        d = self.info.to_dict()
        self.assertEqual(d["serial"], "SLES-54218")
        self.assertEqual(d["version"], "1.02")
        self.assertIsInstance(d["languages"], list)


class TestRpk(unittest.TestCase):
    def _arc(self, name):
        p = os.path.join(ROR_EXTRACT, name)
        if not os.path.exists(p):
            self.skipTest(f"{name} not extracted to {ROR_EXTRACT}")
        with open(p, "rb") as fh:
            return RpkArchive(fh.read())

    def test_mnurun(self):
        a = self._arc("MNUNOW.RPK")
        self.assertGreaterEqual(len(a), 1)
        self.assertIsNotNone(a.get("loading.tm2"))
        data = a.read("loading.tm2")
        self.assertEqual(len(data), 2048)

    def test_evt(self):
        a = self._arc("EVT01.RPK")
        self.assertGreaterEqual(len(a), 1)
        # at least one entry should be readable
        rf = a.files[0]
        data = a.read(rf.path)
        self.assertEqual(len(data), rf.size)


if __name__ == "__main__":
    unittest.main(verbosity=2)
