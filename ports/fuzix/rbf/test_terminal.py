#!/usr/bin/env python3
"""Use a private PTY to check interactive mode and restoration on every exit."""
import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile
import termios
import unittest
from unittest.mock import Mock, patch
import run as launcher


class TerminalTests(unittest.TestCase):
    def test_guest_edits_input_and_host_terminal_is_restored(self):
        for interrupted in (False, True):
            with self.subTest(interrupted=interrupted), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / 'build'
                (build / 'shell').mkdir(parents=True)
                (root / 'target/release').mkdir(parents=True)
                for path in (build / 'shell/kernel.bf', build / 'shell/root.img', root / 'target/release/rbf'):
                    path.touch()
                master, slave = pty.openpty()
                original = termios.tcgetattr(slave)
                stdin = Mock()
                stdin.isatty.return_value = True
                stdin.fileno.return_value = slave
                def guest(command):
                    attributes = termios.tcgetattr(slave)
                    self.assertFalse(attributes[3] & termios.ECHO)
                    self.assertFalse(attributes[3] & termios.ICANON)
                    self.assertTrue(attributes[3] & termios.ISIG)
                    self.assertIn('--tty-stdin', command)
                    self.assertNotIn('--timeout', command)
                    if interrupted:
                        raise KeyboardInterrupt
                    return subprocess.CompletedProcess(command, 7)
                try:
                    with patch.object(launcher, 'ROOT', root), patch.object(sys, 'stdin', stdin), \
                         patch.object(sys, 'argv', ['run.py', '--run-only', '--build', str(build)]), \
                         patch.object(subprocess, 'run', side_effect=guest):
                        if interrupted:
                            with self.assertRaises(KeyboardInterrupt):
                                launcher.main()
                        else:
                            self.assertEqual(launcher.main(), 7)
                    restored = termios.tcgetattr(slave)
                    # Darwin marks canonical input for reprocessing on this
                    # transition. PENDIN is kernel state, not a changed mode.
                    pending = getattr(termios, 'PENDIN', 0)
                    restored[3] &= ~pending
                    original[3] &= ~pending
                    self.assertEqual(restored, original)
                finally:
                    os.close(slave)
                    os.close(master)


if __name__ == '__main__':
    unittest.main()
