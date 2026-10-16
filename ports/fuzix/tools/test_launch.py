#!/usr/bin/env python3
"""Check backend arguments, disk locking and the RBF terminal bridge."""
import argparse
import importlib.util
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import termios
import time
import unittest

LAUNCH = Path(__file__).with_name('launch.py')
spec = importlib.util.spec_from_file_location('launch', LAUNCH)
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class LauncherTests(unittest.TestCase):
    def test_backend_arguments_and_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / 'bios.bf').write_text('BFHOST 1 BANK_SIZE 64000 MEMORY_BANKS 80 '
                'INSTRUCTIONS 100 ENTRY 0 ORIGIN 8 TAPE_CELLS 139264872 END_BFHOST\n')
            args = argparse.Namespace(build=build, vm='/host', backend='rvm',
                vm_args=['--', '--jit', '--headless'])
            command, interactive = launch.host_command(args, build / 'disk')
            self.assertFalse(interactive)
            self.assertIn(str(build / 'bios.bin'), command)
            self.assertIn('--guest-ctrl-c', command)
            self.assertEqual(command[command.index('--memory') + 1], '5120000')
            (build / '.mmu').write_text('1\n')
            protected, _ = launch.host_command(args, build / 'disk')
            self.assertEqual(protected[protected.index('--memory') + 1], '16384000')
            args.backend = 'rbf'
            command, interactive = launch.host_command(args, build / 'disk')
            self.assertFalse(interactive)
            self.assertIn(str(build / 'bios.bf'), command)
            self.assertNotIn('--guest-ctrl-c', command)
            self.assertNotIn('--memory', command)
            self.assertEqual(command[command.index('--max-tape-cells') + 1], '139264872')
            args.vm_args = ['--headless', '--max-tape-cells=200000000']
            command, _ = launch.host_command(args, build / 'disk')
            self.assertNotIn('--max-tape-cells', command)

    def test_terminal_controls_lock_and_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            disk = build / 'disk'
            disk.touch()
            (build / 'bios.bf').write_text('TAPE_CELLS 139264872 END_BFHOST\n')
            fake = build / 'host'
            fake.write_text('#!' + sys.executable + '\n' + '''
import json, os, signal, sys
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
print('READY:' + json.dumps(sys.argv[1:]), flush=True)
while True:
    data = os.read(0, 4096)
    if not data: break
    print('BYTES:' + data.hex(), flush=True)
''')
            fake.chmod(0o755)
            master, slave = pty.openpty()
            saved = termios.tcgetattr(slave)
            cmd = [sys.executable, str(LAUNCH), '--build', str(build), '--disk', str(disk),
                   '--vm', str(fake), '--backend', 'rbf', '--', '--jit']
            child = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave)
            transcript = bytearray()

            def until(needle):
                deadline = time.monotonic() + 10
                while needle not in transcript and time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        transcript.extend(os.read(master, 65536))
                self.assertIn(needle, transcript)

            try:
                until(b'READY:')
                self.assertIn(b'--tty-stdin', transcript)
                blocked = subprocess.run(cmd, capture_output=True, timeout=10)
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn(b'Disk is already in use', blocked.stderr)
                controls = b'\x03\t\x1b[A\x04\n'
                os.write(master, controls)
                until(b'BYTES:' + controls.hex().encode())
                os.write(master, b'\x1d')
                self.assertEqual(child.wait(timeout=10), 0)
                restored = termios.tcgetattr(slave)
                # macOS sets PENDIN when restoring canonical input with queued
                # bytes; it is kernel bookkeeping, not a changed TTY mode.
                restored[3] &= ~getattr(termios, 'PENDIN', 0)
                saved[3] &= ~getattr(termios, 'PENDIN', 0)
                self.assertEqual(restored, saved)
                transcript.clear()
                child = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave)
                until(b'READY:')
                child.terminate()
                self.assertEqual(child.wait(timeout=10), 128 + 15)
                restored = termios.tcgetattr(slave)
                restored[3] &= ~getattr(termios, 'PENDIN', 0)
                self.assertEqual(restored, saved)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
                os.close(master)
                os.close(slave)


if __name__ == '__main__':
    unittest.main()
