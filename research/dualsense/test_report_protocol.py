"""Offline framing controls and byte-checked original Sony call sites."""
import copy
import pathlib
import tempfile
import unittest

from report_protocol import BASE, audit_native, decode_telemetry, fields_from_evidence, telemetry_bytes


def chunks_for(payload):
    assert len(payload) == 336
    return [b"\x81\x70\x01" + bytes([2 if i == 5 else 3]) + payload[56*i:56*(i+1)] + bytes(4)
            for i in range(6)]


class FramingTests(unittest.TestCase):
    def test_boundaries_and_status(self):
        data = bytes(i % 256 for i in range(336))
        chunks = chunks_for(data)
        self.assertEqual(telemetry_bytes(chunks), data)
        for bad in (chunks[:-1], chunks + chunks[:1]):
            with self.assertRaises(ValueError):
                telemetry_bytes(bad)
        for index in range(6):
            for length in (0, 4, 63, 65):
                bad = chunks.copy()
                bad[index] = bytes(length)
                with self.assertRaises(ValueError):
                    telemetry_bytes(bad)
            for offset in range(4):
                bad = chunks.copy()
                value = bytearray(bad[index]); value[offset] ^= 0xff
                bad[index] = bytes(value)
                with self.assertRaises(ValueError):
                    telemetry_bytes(bad)

    def test_unsigned_fields_cross_chunk_and_redaction(self):
        payload = bytearray(336)
        payload[0] = 7
        payload[1:18] = b"PRIVATE-ID-123456"
        fields = [{"name": "peripheralSerialNumber", "signature": {"field_type": "char[]"}}]
        position = 18
        expected = {}
        for index, kind in enumerate(["u4"] * 10 + ["u1", "u2", "u4"]):
            size = {"u1":1,"u2":2,"u4":4}[kind]
            value = (1 << (size*8)) - 1 - index
            name = f"counter{index}"
            fields.append({"name":name,"signature":{"field_type":kind}})
            payload[position:position+size] = value.to_bytes(size,"little")
            position += size
            expected[name] = value
        decoded = decode_telemetry(chunks_for(payload), fields)
        self.assertEqual(decoded["decoded_numeric_fields"], expected)
        self.assertEqual(decoded["format_byte"], 7)
        self.assertEqual(decoded["known_fields_end"], position)
        self.assertTrue(decoded["serial_redacted"])
        self.assertNotIn("PRIVATE", str(decoded))
        bad = copy.deepcopy(fields); bad[1]["signature"]["field_type"] = "i4"
        with self.assertRaises(ValueError):
            decode_telemetry(chunks_for(payload), bad)
        bad = copy.deepcopy(fields); bad[-1]["name"] = bad[1]["name"]
        with self.assertRaises(ValueError):
            decode_telemetry(chunks_for(payload), bad)
        payload[1] = 0
        with self.assertRaises(ValueError):
            decode_telemetry(chunks_for(payload), fields)


class OriginalEvidenceTests(unittest.TestCase):
    def test_named_layout(self):
        path = BASE / "managed-members.json"
        if not path.exists():
            self.skipTest("Original managed inventory unavailable")
        fields = fields_from_evidence(path)
        self.assertEqual(len(fields), 56)
        self.assertEqual(fields[1]["name"], "totalRecordCount")
        self.assertEqual(fields[-1]["name"], "micPushCount")

    def test_native_call_bytes_and_changed_input(self):
        binary = BASE / "payload/Data1/libfwupdater.dll"
        disassembly = BASE / "libfwupdater-disassembly.txt"
        if not binary.exists() or not disassembly.exists():
            self.skipTest("Original native evidence unavailable")
        audit = audit_native(binary, disassembly)
        self.assertEqual(len(audit["direct_call_sites_rva"]["HidD_GetFeature_thunk"]), 10)
        self.assertEqual(audit["direct_call_sites_rva"]["factory_selector_sender"], ["0xfde1"])
        with tempfile.TemporaryDirectory() as directory:
            changed = pathlib.Path(directory) / "changed.dll"
            data = bytearray(binary.read_bytes()); data[-1] ^= 1; changed.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Unsupported native"):
                audit_native(changed, disassembly)
            changed = pathlib.Path(directory) / "changed-disassembly.txt"
            text = disassembly.read_text()
            self.assertIn("180009e3f: e8 36 89 00 00", text)
            # Keep the same decoded target while supplying different bytes.
            changed.write_text(text.replace("180009e3f: e8 36 89 00 00", "180009e40: e8 35 89 00 00"))
            with self.assertRaisesRegex(ValueError, "disagree"):
                audit_native(binary, changed)


if __name__ == "__main__":
    unittest.main()
