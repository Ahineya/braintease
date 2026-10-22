#!/usr/bin/env python3
"""Controls for the finite, file-only resource-key hypothesis probe."""

import base64
import shutil
import unittest

from probe_resource_keys import encoded_candidates, openssl_run, resource_key
from unpack_resources import BASE, resources


class CandidateChecks(unittest.TestCase):
    def test_encoded_lengths_and_rejections(self):
        rows = [{"key": 1, "value": "ab" * 16},
                {"key": 2, "value": base64.b64encode(bytes(range(24))).decode()},
                {"key": 3, "value": "not a cryptographic key"},
                {"key": 4, "value": base64.b64encode(b"short").decode()}]
        candidates = encoded_candidates(rows)
        self.assertEqual([(name, len(key)) for name, key, _ in candidates],
                         [("string-hex-1", 16), ("string-base64-2", 24)])
        self.assertEqual(candidates[0][1], bytes.fromhex("ab" * 16))
        self.assertEqual(candidates[1][1], bytes(range(24)))


class CryptoControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.openssl = shutil.which("openssl")
        if not cls.openssl:
            raise unittest.SkipTest("OpenSSL required")

    def test_aes_ecb_known_vector(self):
        # FIPS 197 AES-128 example, independent of Sony resource inputs.
        ciphertext = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
        output = openssl_run(self.openssl, ["enc", "-d", "-aes-128-ecb", "-nopad", "-K",
                                            "000102030405060708090a0b0c0d0e0f"], ciphertext)
        self.assertEqual(output.hex(), "00112233445566778899aabbccddeeff")

    def test_cmac_known_vector(self):
        # RFC 4493 example 1, empty message.
        output = openssl_run(self.openssl, ["mac", "-macopt", "cipher:AES-128-CBC", "-macopt",
                                            "hexkey:2b7e151628aed2a6abf7158809cf4f3c", "CMAC"], b"")
        self.assertEqual(output.decode().strip().lower(), "bb1d6929e95937287fa37d129b756746")

    def test_original_resource_key_matches_recorded_ciphertext(self):
        binary = BASE / "payload/Data1/playstationaccessories.dll"
        if not binary.exists():
            self.skipTest("Pinned updater not extracted")
        raw = resources(binary.read_bytes(), [("leyD", 0)])["leyD"]
        key, iv = resource_key(raw)
        # First clear block independently recorded during resource recovery.
        output = openssl_run(self.openssl, ["enc", "-d", "-aes-256-cbc", "-nopad", "-K",
                                            key.hex(), "-iv", iv.hex()], raw[67:83])
        self.assertEqual(output.hex(), "7ce09d0627ff83bd8d0ff62e7bb077ef")


if __name__ == "__main__":
    unittest.main()
