#!/usr/bin/env python3
"""Check recovered updater references against independent original-byte decoding."""

import collections
import json
from pathlib import Path
import tempfile
import unittest

from trace_updater import BASE, Trace, coordinates


class CoordinateChecks(unittest.TestCase):
    def test_known_initializer(self):
        # Actual five-instruction initializer 0x06000c01, independently inspected.
        self.assertEqual(coordinates([942034447, 737618165, 1144309078], "method"),
                         ("0x020001b5", "0x0a000117", "0x04000a56", None))


class CorpusChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (BASE / "managed-members.json").exists():
            raise unittest.SkipTest("Local REA evidence not generated")
        cls.trace = Trace(BASE / "managed-members.json", BASE / "payload/Data1/playstationaccessories.dll")
        cls.rows = cls.trace.recover()

    def test_all_factory_families_and_native_targets(self):
        self.assertEqual(collections.Counter(x["factory"] for x in self.rows), {
            "0x06000a68": 1395, "0x06000a69": 78, "0x06000a6a": 223, "0x06000a6b": 23,
        })
        expected = {"0x0600002e": "PSAccessories.LibFWupdater.StartUpdate",
                    "0x0600002f": "PSAccessories.LibFWupdater.WriteUpdateImage",
                    "0x06000030": "PSAccessories.LibFWupdater.VerifyUpdateImage",
                    "0x06000031": "PSAccessories.LibFWupdater.FinalizeUpdate"}
        self.assertEqual({x["target"]: x["name"] for x in self.rows if x["target"] in expected}, expected)

    def test_file_loader_boundaries(self):
        calls = {e["target_label"] for e in self.trace.edges() if e["caller_token"] == "0x06000834"}
        self.assertTrue({"System.IO.FileStream..ctor", "System.IO.BinaryReader..ctor",
                         "System.IO.MemoryStream..ctor", "System.IO.Stream.CopyTo",
                         "System.IO.MemoryStream.ToArray", "\ue046.set_Binary"}.issubset(calls))
        # Verify selected original CIL bodies independently, not only the REA names.
        for token in ["0x06000818", "0x0600081b", "0x0600081c", "0x06000834"]:
            self.trace.body(token)

    def test_mismatched_original_bytes_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "other.dll"
            path.write_bytes(b"not the verified Sony assembly")
            with self.assertRaisesRegex(ValueError, "assembly bytes"):
                Trace(BASE / "managed-members.json", path)

    def test_evidence_cil_digest_is_cross_checked(self):
        token = "0x0600081b"
        metadata = self.trace.methods[token]["body"]
        original = metadata["il_sha256"]
        try:
            metadata["il_sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "disagrees"):
                self.trace.body(token)
        finally:
            metadata["il_sha256"] = original


if __name__ == "__main__":
    unittest.main()
