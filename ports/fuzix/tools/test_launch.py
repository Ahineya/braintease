#!/usr/bin/env python3
"""Check MMU-only launch policy, early retirement failures and disk locking."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

LAUNCH = Path(__file__).with_name('launch.py')
spec = importlib.util.spec_from_file_location('launch', LAUNCH)
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        (self.build / '.mmu').write_text('1\n')

    def test_network_policy(self):
        with patch.dict(os.environ, {}, clear=True):
            args = argparse.Namespace(build=self.build, vm='/host', backend='rvm', vm_args=[])
            command = launch.host_command(args, 'disk')
            self.assertEqual(command[command.index('--net') + 1], 'shared')
            self.assertIn('--net-eager', command)
            os.environ['RIPPLE_NET'] = 'shared'
            command = launch.host_command(args, 'disk')
            self.assertEqual(command[command.index('--net') + 1], 'shared')
            self.assertEqual(command.count('--net-eager'), 1)
            os.environ['RIPPLE_NET'] = 'off'
            command = launch.host_command(args, 'disk')
            self.assertEqual(command[command.index('--net') + 1], 'off')
            self.assertNotIn('--net-eager', command)
            args.vm_args = ['--net=host']
            command = launch.host_command(args, 'disk')
            self.assertIn('--net=host', command)
            self.assertNotIn('--net', command)
            self.assertIn('--net-eager', command)
            args.vm_args = ['--net', '/tmp/ethernet']
            command = launch.host_command(args, 'disk')
            self.assertEqual(command[command.index('--net') + 1], '/tmp/ethernet')
            self.assertNotIn('--net-eager', command)
            args.vm_args = ['--net', 'off']
            command = launch.host_command(args, 'disk')
            self.assertEqual(command[command.index('--net') + 1], 'off')
            self.assertNotIn('--net-eager', command)

    def test_graphics_close_policy(self):
        args = argparse.Namespace(build=self.build, vm="/host", backend="rvm", vm_args=["--visual"])
        command = launch.host_command(args, "disk")
        self.assertIn("--guest-window-close", command)
        args.vm_args = ["--headless"]
        command = launch.host_command(args, "disk")
        self.assertNotIn("--guest-window-close", command)

    def test_unix_audio_policy(self):
        args = argparse.Namespace(build=self.build, vm='/host', backend='rvm', vm_args=[])
        command = launch.host_command(args, 'disk')
        self.assertEqual(command[command.index('--audio') + 1], 'on')
        args.vm_args = ['--headless']
        command = launch.host_command(args, 'disk')
        self.assertEqual(command[command.index('--audio') + 1], 'auto')
        args.vm_args = ['--audio=off']
        command = launch.host_command(args, 'disk')
        self.assertIn('--audio=off', command)
        self.assertNotIn('--audio', command)

    def test_rvm_arguments_and_overrides(self):
        args = argparse.Namespace(build=self.build, vm='/host', backend='rvm',
                                  vm_args=['--', '--jit', '--headless'])
        command = launch.host_command(args, self.build / 'disk')
        self.assertIn(str(self.build / 'bios.bin'), command)
        self.assertIn('--guest-ctrl-c', command)
        self.assertEqual(command[command.index('--memory') + 1], '16384000')
        for override in (['--memory', '32768000'], ['--memory=32768000'], ['-m', '32768000']):
            args.vm_args = override
            command = launch.host_command(args, 'disk')
            self.assertNotIn('16384000', command)
            for option in override:
                self.assertIn(option, command)

    def test_retired_build_targets(self):
        port = LAUNCH.parents[1]
        stale = self.build / 'bios.bf'
        stale.write_text('obsolete export')
        for options in (['MMU=0', 'images'], ['MMU=2', 'kernel'], ['bf'], [str(stale)]):
            with self.subTest(options=options):
                result = subprocess.run(['make', '-s', '-C', str(port),
                    f'BUILD={self.build}', *options], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('retired', result.stderr)
                self.assertEqual(stale.read_text(), 'obsolete export')
                self.assertFalse((self.build / 'kernel').exists())

    def test_retired_launches_do_not_touch_disk(self):
        disk = self.build / 'disk'
        disk.write_bytes(b'user disk must remain untouched')
        cmd = [sys.executable, str(LAUNCH), '--build', str(self.build),
               '--disk', str(disk), '--vm', '/missing-vm', '--update', '--fresh']
        for profile in ('0', '', None):
            marker = self.build / '.mmu'
            if profile is None:
                marker.unlink()
            else:
                marker.write_text(profile)
            with self.subTest(profile=profile):
                result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('MMU=0 is retired', result.stderr)
                self.assertEqual(disk.read_bytes(), b'user disk must remain untouched')
                self.assertFalse(Path(str(disk) + '.lock').exists())
        (self.build / '.mmu').write_text('1\n')
        result = subprocess.run([*cmd, '--backend', 'rbf'], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FUZIX/RBF is retired', result.stderr)
        self.assertFalse(Path(str(disk) + '.lock').exists())

    def test_shell_retirement_and_argument_forwarding(self):
        script = LAUNCH.parents[3] / 'scripts/run-fuzix.sh'
        env = dict(os.environ, FUZIX_BUILD=str(self.build))
        for option in ('rbf', '--rbf'):
            result = subprocess.run([str(script), option, '--build-only'],
                env=env, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('FUZIX/RBF is retired', result.stderr)
            self.assertNotIn('Checking compiler', result.stderr)
        (self.build / 'bios.bin').touch()
        result = subprocess.run([str(script), '--run-only', '--input', 'rbf'],
            env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('disk not found', result.stderr)
        (self.build / '.mmu').write_text('0\n')
        result = subprocess.run([str(script), '--run-only'], env=env,
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('MMU=0 is retired', result.stderr)

    def test_disk_lock_survives_exec_and_releases_on_exit(self):
        disk = self.build / 'disk'
        root = LAUNCH.parents[3]
        subprocess.run(['node', '--input-type=module', '-e', """
import {rootImage, atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
import {Image} from './ports/fuzix/tools/image.mjs';
const image=fat16Image([{name:'ROOTFS.IMG',data:rootImage([],256)}],{imageFactory:n=>new Image(n)});
atomicWrite(process.argv[1],image);
""", str(disk)], cwd=root, check=True)

        fake = self.build / 'host'
        fake.write_text('#!' + sys.executable + '\n' + """
import signal, sys
print('READY', flush=True)
signal.pause()
""")
        fake.chmod(0o755)
        cmd = [sys.executable, str(LAUNCH), '--build', str(self.build), '--disk', str(disk),
               '--vm', str(fake), '--', '--net', 'off']
        child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline(), 'READY\n')
            blocked = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn('Disk is already in use', blocked.stderr)
        finally:
            child.terminate()
            child.communicate(timeout=10)
        # An exec'd VM inherits the lock; its death releases it for the next boot.
        unlocked = subprocess.run([sys.executable, str(LAUNCH), '--build', str(self.build),
            '--disk', str(disk), '--vm', '/usr/bin/true', '--', '--net', 'off'],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(unlocked.returncode, 0, unlocked.stderr)


if __name__ == '__main__':
    unittest.main()
