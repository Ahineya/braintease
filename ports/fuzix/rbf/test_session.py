#!/usr/bin/env python3
"""Disposable launcher disks are independent and removed on every exit path."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import run as launcher


class SessionTests(unittest.TestCase):
    def test_each_launch_starts_fresh_and_preserves_the_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            build=root/'build'
            profile=build/'shell'
            profile.mkdir(parents=True)
            (root/'target/release').mkdir(parents=True)
            (root/'target/release/rbf').touch()
            (profile/'kernel.bf').touch()
            template=profile/'root.img'
            template.write_bytes(b'fresh template')
            disks=[]
            telemetry_expected=False
            interrupted=False
            def guest(command):
                disk=Path(command[command.index('--disk')+1])
                self.assertNotEqual(disk,template)
                self.assertEqual(disk.read_bytes(),b'fresh template')
                self.assertEqual('--telemetry-lite' in command, telemetry_expected)
                disks.append(disk)
                disk.write_bytes(b'guest changed this disk')
                if interrupted:
                    raise KeyboardInterrupt
                return subprocess.CompletedProcess(command,3)
            stdin=Mock()
            stdin.isatty.return_value=False
            with patch.object(launcher,'ROOT',root), patch.object(sys,'stdin',stdin), \
                 patch.object(sys,'argv',['run.py','--run-only','--build',str(build)]), \
                 patch.object(subprocess,'run',side_effect=guest):
                self.assertEqual(launcher.main(),3)
                telemetry_expected=True
                with patch.object(sys,'argv',['run.py','--run-only','--build',str(build),'--telemetry']):
                    self.assertEqual(launcher.main(),3)
                telemetry_expected=False
                interrupted=True
                with self.assertRaises(KeyboardInterrupt):
                    launcher.main()
            self.assertEqual(template.read_bytes(),b'fresh template')
            self.assertEqual(len(set(disks)),3)
            self.assertTrue(all(not disk.exists() for disk in disks))
            self.assertFalse(list(profile.glob('session-*')))


if __name__=='__main__':
    unittest.main()
