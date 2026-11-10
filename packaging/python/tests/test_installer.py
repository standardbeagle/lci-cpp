"""Platform -> release-asset selection for the pip installer.

Run: PYTHONPATH=packaging/python python3 packaging/python/tests/test_installer.py
"""

import sys
import unittest
from unittest import mock

from lci_cli import installer


def _matcher(platform_name: str, machine: str):
    with mock.patch.object(sys, "platform", platform_name), \
         mock.patch.object(installer.platform, "machine", return_value=machine):
        return installer._asset_matcher()


class AssetMatcherTest(unittest.TestCase):
    def test_apple_silicon_gets_the_darwin_asset(self):
        match = _matcher("darwin", "arm64")
        self.assertTrue(match("lci-1.2.3-Darwin.tar.gz"))

    # The only macOS release artifact is built on arm64 runners; an Intel Mac
    # must be refused up front, not handed a binary that cannot exec.
    def test_intel_mac_is_refused(self):
        with self.assertRaises(installer.InstallError) as ctx:
            _matcher("darwin", "x86_64")
        self.assertIn("Apple Silicon", str(ctx.exception))

    def test_linux_x86_64_gets_the_linux_asset(self):
        match = _matcher("linux", "x86_64")
        self.assertTrue(match("lci-1.2.3-Linux.tar.gz"))
        self.assertFalse(match("lci-1.2.3-Darwin.tar.gz"))



class ExpectedSha256Test(unittest.TestCase):
    NAME = "lci-1.2.3-Linux.tar.gz"

    # A suffix match let any line whose filename merely ENDS with the asset
    # name supply the hash.
    def test_suffix_lookalike_does_not_supply_the_hash(self):
        sums = ("a" * 64 + "  evil-" + self.NAME + "\n"
                + "b" * 64 + "  " + self.NAME + "\n")
        self.assertEqual(installer._expected_sha256(sums, self.NAME), "b" * 64)

    def test_binary_mode_marker_is_accepted(self):
        sums = "C" * 64 + " *" + self.NAME + "\n"
        self.assertEqual(installer._expected_sha256(sums, self.NAME), "c" * 64)

    def test_missing_entry_is_none(self):
        sums = "a" * 64 + "  evil-" + self.NAME + "\n"
        self.assertIsNone(installer._expected_sha256(sums, self.NAME))

if __name__ == "__main__":
    unittest.main()
