#!/usr/bin/env python3
"""Check raw-terminal Ctrl+C with actual VM processes (Unix; build rvm first)."""
import fcntl
import json
import os
from pathlib import Path
import pty
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
RVM = Path(os.environ.get("RVM", ROOT / "target/release/rvm"))


class TerminalInterrupt(unittest.TestCase):
    def test_ctrl_c_without_guest_cooperation(self):
        for jit in (False, True):
            for reads_input in (False, True):
                with self.subTest(jit=jit, reads_input=reads_input):
                    self.check_interrupt(jit, reads_input)

    def check_interrupt(self, jit, reads_input):
        # Enter TTY mode, then loop forever, optionally polling input status.
        code = [(0x0E, 5, 1, 0), (0x0E, 6, 6, 0), (0x12, 5, 0, 6)]
        if reads_input:
            code += [(0x0E, 6, 3, 0), (0x11, 7, 0, 6), (0x15, 0, 0, 0xFFFE)]
        else:
            code += [(0x15, 0, 0, 0xFFFF)]
        image = b"RLINK" + struct.pack("<HII", 64, 0, len(code))
        image += b"".join(struct.pack("<BBHHH", op, op, a, b, c) for op, a, b, c in code)
        image += struct.pack("<I", 0)
        with tempfile.TemporaryDirectory(prefix="rvm-terminal-") as directory:
            root = Path(directory)
            binary = root / "loop.bin"
            report = root / "report.json"
            attributes = root / "terminal.json"
            binary.write_bytes(image)
            master, slave = pty.openpty()
            before = termios.tcgetattr(slave)
            process = None
            try:
                def terminal_session():
                    os.setsid()
                    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

                args = [str(RVM), str(binary), "--memory", "4096",
                        "--disk", str(root / "disk.img"), "--report", str(report)]
                if jit:
                    args.append("--jit")
                # Keep a session leader alive while RVM exits. macOS revokes a
                # PTY when its session leader exits, even with other open FDs.
                wrapper = (
                    "import json, subprocess, sys, termios; "
                    "status = subprocess.call(sys.argv[2:]); "
                    "state = termios.tcgetattr(0); "
                    "state[6] = [v[0] if isinstance(v, bytes) else v for v in state[6]]; "
                    "open(sys.argv[1], 'w').write(json.dumps(state)); sys.exit(status)"
                )
                process = subprocess.Popen([sys.executable, "-c", wrapper, str(attributes), *args],
                                           stdin=slave, stdout=slave, stderr=slave,
                                           preexec_fn=terminal_session, close_fds=True)
                deadline = time.monotonic() + 5
                while termios.tcgetattr(slave)[3] & termios.ICANON:
                    self.assertIsNone(process.poll(), "VM stopped before entering raw mode")
                    self.assertLess(time.monotonic(), deadline, "VM did not enter raw mode")
                    time.sleep(0.01)
                # Let the loop become hot before testing the native path.
                time.sleep(0.1)
                os.write(master, b"\x03")
                self.assertEqual(process.wait(timeout=3), 0)
                result = json.loads(report.read_text())
                self.assertEqual(result["stop_reason"], "host_stop")
                before[6] = [v[0] if isinstance(v, bytes) else v for v in before[6]]
                self.assertEqual(json.loads(attributes.read_text()), before, "terminal was not restored")
                if jit:
                    self.assertGreater(result["jit"]["native_instructions"], 0)
            finally:
                if process is not None and process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                os.close(master)
                os.close(slave)


if __name__ == "__main__":
    unittest.main()
