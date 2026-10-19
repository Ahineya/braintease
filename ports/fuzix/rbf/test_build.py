#!/usr/bin/env python3
"""Check the launcher's real progress pipeline without rebuilding Rust tools."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BuildOutputTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='fuzix-bf-progress-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.build = self.directory / 'build'
        tools = self.directory / 'tools'
        tools.mkdir()
        for name, source in {
            'cargo': '''
import os, sys, time
from pathlib import Path
print('private compiler detail', flush=True)
Path(os.environ['STARTED']).touch()
while not Path(os.environ['RELEASE']).exists(): time.sleep(.01)
code = int(os.environ.get('FAIL_CODE', '0'))
if code: print('error: simulated compiler failure', flush=True)
sys.exit(code)
''',
            'make': '''
import sys
from pathlib import Path
build = Path(next(a[6:] for a in sys.argv if a.startswith('BUILD=')))
print('Build: Linking reference kernel', flush=True)
print('Build: Exporting Brainfuck boot kernel', flush=True)
(build / 'kernel.bf').touch()
(build / 'root.img').touch()
(build / 'shell').mkdir()
(build / 'shell/kernel.bf').touch()
(build / 'shell/root.img').touch()
''',
        }.items():
            tool = tools / name
            tool.write_text(f'#!{sys.executable}\n' + source)
            tool.chmod(0o755)
        self.release = self.directory / 'release'
        self.started = self.directory / 'started'
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        STARTED=str(self.started), RELEASE=str(self.release), FAIL_CODE='0')
        self.command = [str(ROOT / 'scripts/run-fuzix-bf.sh'), '--build', str(self.build), '--build-only']

    def test_progress_is_visible_before_build_completes_and_full_log_is_retained(self):
        child = subprocess.Popen(self.command, env=self.env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
        prefix = bytearray()
        try:
            deadline = time.monotonic() + 10
            while b'Building RCC and RBF host tools' not in prefix:
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, prefix.decode())
                self.assertTrue(select.select([child.stderr], [], [], remaining)[0])
                chunk = os.read(child.stderr.fileno(), 65536)
                self.assertTrue(chunk, prefix.decode())
                prefix.extend(chunk)
            while not self.started.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(self.started.exists())
            self.assertIsNone(child.poll(), 'build completed before live output was observed')
            self.release.touch()
            stdout, stderr = child.communicate(timeout=10)
            self.assertEqual(child.returncode, 0, stderr.decode())
            terminal = (prefix + stderr).decode()
            for phase in ('Building assembler and linker', 'Building Riplix boot kernel and images',
                          'Linking reference kernel', 'Exporting Brainfuck boot kernel', 'Build complete'):
                self.assertIn(phase, terminal)
            self.assertNotIn('private compiler detail', terminal)
            self.assertEqual((self.build / 'build.log').read_text().count('private compiler detail'), 2)
            self.assertIn('Built ', stdout.decode())
        finally:
            self.release.touch()
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                child.communicate(timeout=10)

    def test_failure_is_reported_and_prevents_later_build_steps(self):
        self.release.touch()
        result = subprocess.run(self.command, env=dict(self.env, FAIL_CODE='17'),
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertIn('error: simulated compiler failure', result.stderr)
        self.assertIn('Build failed (exit 17)', result.stderr)
        self.assertNotIn('Building assembler and linker', result.stderr)
        self.assertFalse((self.build / 'kernel.bf').exists())
        self.assertIn('private compiler detail', (self.build / 'build.log').read_text())


if __name__ == '__main__':
    unittest.main()
