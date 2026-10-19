#!/usr/bin/env python3
"""Execute the same boot kernel on RVM and the real BF CPU, including failures."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import select
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build/fuzix-rbf'
SUCCESS = 'RBF boot OK: PID 1, root mounted, file verified.'


class BootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='fuzix-bf-')
        cls.directory = Path(cls.temporary.name)
        subprocess.run(['node', '--input-type=module', '-e', '''
import path from 'node:path';
import {rootImage, atomicWrite} from './ports/fuzix/tools/disk.mjs';
const directory = process.argv[1];
for (const [name, files] of [
  ['good', [{name:'/BOOT', mode:0o100444, data:Buffer.from('Riplix kernel running in Brainfuck.\\n')}]],
  ['missing', []],
  ['wrong', [{name:'/BOOT', mode:0o100444, data:Buffer.from('not the expected boot file\\n')}]],
  ['trailing', [{name:'/BOOT', mode:0o100444, data:Buffer.from('Riplix kernel running in Brainfuck.\\nextra')}]],
]) atomicWrite(path.join(directory, name+'.img'), rootImage(files,128,32));
''', str(cls.directory)], cwd=ROOT, check=True)
        bad = bytearray((cls.directory / 'good.img').read_bytes())
        bad[2048:2050] = b'\0\0'
        (cls.directory / 'bad-magic.img').write_bytes(bad)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_kernel(self, disk, backend, jit=False):
        source = self.directory / f'{disk}.img'
        before = hashlib.sha256(source.read_bytes()).digest()
        command = [str(ROOT / 'target/release' / backend),
                   str(BUILD / ('kernel.bf' if backend == 'rbf' else 'kernel.bin')),
                   '--disk', str(source), '--headless', '--net', 'off',
                   '--timeout', '60', '--exit-code']
        report = self.directory / f'{disk}-{backend}-{jit}.json'
        if backend == 'rbf':
            command += ['--report', str(report)]
        else:
            command += ['--memory', '65536']
        if jit:
            command.append('--jit')
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
        self.assertEqual(before, hashlib.sha256(source.read_bytes()).digest(), 'boot wrote to read-only disk')
        data = json.loads(report.read_text()) if backend == 'rbf' else None
        return result, data

    def test_boot_matches_rvm_in_bf_interpreter_and_jit(self):
        reference, _ = self.run_kernel('good', 'rvm')
        self.assertEqual(reference.returncode, 0, reference.stderr)
        self.assertIn('Mounting root fs (root_dev=0, ro): OK', reference.stdout)
        self.assertIn(SUCCESS, reference.stdout)
        banner=(ROOT / "ports/fuzix/banner.tmp.txt").read_text().rstrip("\n")
        self.assertIn(banner, reference.stdout.replace("\r", ""))
        self.assertIn("Derived from FUZIX.", reference.stdout)
        instruction_counts = []
        for jit in (False, True):
            with self.subTest(jit=jit):
                result, report = self.run_kernel('good', 'rbf', jit)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, reference.stdout)
                self.assertEqual(report['backend'], 'brainfuck')
                self.assertEqual(report['stop_reason'], 'halted')
                self.assertEqual(report['guest_result'], 0)
                self.assertTrue(report['success'])
                self.assertGreater(report['ripple_instructions'], 1000)
                self.assertEqual(report['layout']['memory_banks'], 4)
                self.assertEqual(report['frames'], 0)
                instruction_counts.append(report['ripple_instructions'])
                if jit:
                    self.assertGreater(report['jit']['compiled_regions'], 0)
                    self.assertGreater(report['jit']['native_dispatches'], 0)
        self.assertEqual(*instruction_counts)

    def test_bad_disks_fail_without_success_or_writes(self):
        for disk, reason in [('bad-magic', 'no root'), ('missing', 'BOOT file missing'),
                             ('wrong', 'BOOT file contents'), ('trailing', 'BOOT file contents')]:
            reference, _ = self.run_kernel(disk, 'rvm')
            self.assertEqual(reference.returncode, 1, reference.stderr)
            for backend, jit in [('rbf', True)]:
                with self.subTest(disk=disk, backend=backend):
                    result, report = self.run_kernel(disk, backend, jit)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertEqual(result.stdout, reference.stdout)
                    self.assertIn(f'panic: {reason}', result.stdout)
                    self.assertIn('RBF boot FAILED', result.stdout)
                    self.assertNotIn(SUCCESS, result.stdout)
                    self.assertEqual(report['guest_result'], 1)
                    self.assertEqual(report['stop_reason'], 'halted')

    def test_launcher_isolated_paths_and_early_errors(self):
        script = ROOT / 'scripts/run-fuzix-bf.sh'
        missing = self.directory / 'missing build'
        for options, reason in [(['--run-only'], 'missing'),
                                (['--build-only', '--run-only'], 'not allowed'),
                                (['--disk', str(self.directory / 'good.img')], '--disk is not supported'),
                                (['--no-jit', '--jit'], 'conflicts')]:
            with self.subTest(options=options):
                result = subprocess.run([str(script), '--build', str(missing), *options],
                                        cwd=ROOT, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stderr)
                self.assertFalse(missing.exists())
        result = subprocess.run([str(script), '--build', str(BUILD), '--run-only',
                                 '--no-telemetry', '--boot-check'], cwd=ROOT, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(SUCCESS, result.stdout)
        self.assertEqual(json.loads((BUILD / 'boot.json').read_text())['guest_result'], 0)

    def test_build_waits_without_truncating_another_build_log(self):
        build = self.directory / 'locked-build'
        build.mkdir()
        log = build / 'build.log'
        log.write_text('other build is still writing\n')
        with (build / '.build.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            child = subprocess.Popen([str(ROOT / 'scripts/run-fuzix-bf.sh'),
                '--build', str(build), '--build-only'], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertTrue(select.select([child.stderr], [], [], 10)[0], 'launcher did not report waiting')
                self.assertIn('Waiting for another Riplix/BF build', child.stderr.readline())
                self.assertEqual(log.read_text(), 'other build is still writing\n')
                self.assertIsNone(child.poll())
                self.assertFalse((build / 'kernel').exists())
            finally:
                child.terminate()
                child.communicate(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=BUILD)
    args, rest = parser.parse_known_args()
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__, *rest])
