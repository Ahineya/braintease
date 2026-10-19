#!/usr/bin/env python3
"""Check standalone-service rejection before invoking a guest OS toolchain."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build_os


class StandaloneConsoleTests(unittest.TestCase):
    def test_host_input_and_mmio_are_rejected_for_both_guest_targets(self):
        for target in ("fuzix", "fuse-x"):
            for symbol in ("rcc_hosted_input", "mmio_read"):
                with self.subTest(target=target, symbol=symbol), tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary)
                    source, output = directory / "main.rfl", directory / "app"
                    source.write_text("fn main() {}\n")
                    output.write_bytes(b"previous executable")
                    arguments = ["build_os.py", str(source), "--os", target, "-o", str(output)]
                    ir = f"declare @{symbol}() -> i16\n".encode()
                    with patch("sys.argv", arguments), patch.object(build_os, "run", return_value=ir) as compiler:
                        with self.assertRaisesRegex(RuntimeError, "cannot run in an OS process"):
                            build_os.main()
                    self.assertEqual(compiler.call_count, 1, "must stop before guest linking")
                    self.assertEqual(output.read_bytes(), b"previous executable")
                    self.assertFalse(list(directory.glob(".rfl-build-*")))


if __name__ == "__main__":
    unittest.main()
