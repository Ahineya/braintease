#!/usr/bin/env python3
"""Physical /init loader, syscall ABI and polled shell on RVM and the BF JIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import tempfile
import time
import unittest
from check_image import INIT_CODE, APP_CODE

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build/fuzix-rbf'
HERE = Path(__file__).resolve().parent
BACKENDS = ('rvm', 'rbf')


def run(command, **kwargs):
    result = subprocess.run([str(v) for v in command], cwd=ROOT,
                            capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise AssertionError(f'{command}\n{result.stdout}\n{result.stderr}')
    return result


class ShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fuzix-bf-shell-')
        cls.directory = Path(cls.temp.name)
        cls.profile = BUILD / 'shell'
        asm = ROOT / 'src/ripple-asm/target/release'
        def compile_program(source, name, base=INIT_CODE):
            fixture = cls.directory / name
            run([ROOT / 'target/release/rcc', 'compile', source,
                 '-o', fixture.with_suffix('.asm'), '--bank-size', '16384',
                 '-I' + str(ROOT / 'ports/fuzix/ripple'), '-I' + str(ROOT / 'runtime/include')])
            run([asm / 'rasm', 'assemble', fixture.with_suffix('.asm'), '-o', fixture.with_suffix('.pobj'),
                 '--bank-size', '16384', '--max-immediate', '65535'])
            run([asm / 'rlink', cls.profile / 'user/entry.pobj', fixture.with_suffix('.pobj'),
                 cls.profile / 'user/userlib.pobj', '-o', fixture.with_suffix('.sys'), '-f', 'sys',
                 '--code-bank-base', str(base), '--gp-bank', '3', '--sb-bank', '4', '--bank-size', '16384'])
            return fixture.with_suffix('.sys').read_bytes()
        exit_source = cls.directory / 'exit.c'
        exit_source.write_text('int main(void) { return 7; }\n')
        good = (cls.profile / 'init.sys').read_bytes()
        files = {'good': good, 'abi': compile_program(HERE / 'tests/abi-check.c', 'abi'),
                 'exit': compile_program(exit_source, 'exit'),
                 'process': compile_program(HERE / 'tests/process-check.c', 'process'),
                 'write': compile_program(HERE / 'tests/write-check.c', 'write')}
        programs = {name: str(cls.profile / f'bin/{name}.sys') for name in ('cat', 'ls', 'uname')}
        argcheck = cls.directory / 'argcheck.sys'
        argcheck.write_bytes(compile_program(HERE / 'tests/argcheck.c', 'argcheck', APP_CODE))
        programs['argcheck'] = str(argcheck)
        cases = [('magic', 0, b'NOPE'), ('bank', 4, struct.pack('<H', 64000)),
                 ('placement', 6, struct.pack('<H', 7)), ('gp', 8, struct.pack('<H', 1)),
                 ('entry', 12, struct.pack('<I', 0xffffffff)),
                 ('empty', 16, struct.pack('<I', 0)),
                 ('huge-code', 16, struct.pack('<I', 0xffffffff)),
                 ('huge-data', 20, struct.pack('<I', 16385))]
        for name, offset, value in cases:
            data = bytearray(good)
            data[offset:offset + len(value)] = value
            files[name] = data
        files['short'] = good[:20]
        files['truncated'] = good[:-1]
        files['trailing'] = good + b'x'
        # Enter on the last instruction of the first init bank, then fall through.
        # LI RV0,0; HALT. All preceding instructions are unreachable HALTs.
        code = bytearray((16384 + 1) * 8)
        code[16383 * 8:16384 * 8] = struct.pack('<BBHHH', 0x0e, 0, 5, 0, 0)
        files['cross-bank'] = struct.pack('<4s4H3I', b'SYS1', 16384, INIT_CODE, 3, 4,
                                          16383, 16385, 16384) + code + bytes(16384)
        for name, data in files.items():
            (cls.directory / f'{name}.sys').write_bytes(data)
        manifest = [{'name': name, 'source': str(cls.directory / f'{name}.sys')}
                    for name in files]
        manifest += [{'name': 'missing', 'source': None},
                     {'name': 'no-exec', 'source': str(cls.profile / 'init.sys'), 'mode': 0o100444}]
        run(['node', '--input-type=module', '-e', '''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
for(const item of JSON.parse(process.argv[2])) {
  const files=[{name:'/BOOT',mode:0o100444,data:Buffer.from('FUZIX kernel running in Brainfuck.\\n')}];
  for(const [name,source] of Object.entries(JSON.parse(process.argv[3])))
    files.push({name:'/bin/'+name,mode:0o100555,data:fs.readFileSync(source)});
  files.push({name:'/bin/bad',mode:0o100555,data:Buffer.from('bad')});
  files.push({name:'/bin/noexec',mode:0o100444,data:Buffer.from('not executable')});
  if(item.source)files.push({name:'/init',mode:item.mode??0o100555,data:fs.readFileSync(item.source)});
  atomicWrite(process.argv[1]+'/'+item.name+'.img',rootImage(files,1024,32));
}
''', cls.directory, json.dumps(manifest), json.dumps(programs)])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def guest(self, disk, backend='rvm', commands=''):
        template = self.directory / f'{disk}.img'
        before = hashlib.sha256(template.read_bytes()).digest()
        image = self.directory / f'{disk}-{backend}.working.img'
        shutil.copyfile(template, image)
        report = self.directory / f'{disk}-{backend}.json'
        command = [ROOT / 'target/release' / backend,
                   self.profile / ('kernel.bf' if backend == 'rbf' else 'kernel.bin'),
                   '--disk', image, '--headless', '--net', 'off', '--exit-code',
                   '--input', commands, '--timeout', '360', '--report', report]
        command += ['--jit'] if backend == 'rbf' else ['--memory', '131072']
        result = subprocess.run([str(v) for v in command], cwd=ROOT,
                                capture_output=True, text=True, timeout=390)
        self.assertEqual(before, hashlib.sha256(template.read_bytes()).digest())
        return result, json.loads(report.read_text())

    def test_commands_match_on_rvm_and_bf(self):
        commands = ('uname -a\necho written-in-guest > /tmp/demo\n'
                    'echo appended >> /tmp/demo\ncat /tmp/demo\nls /bin\n'
                    'cd /dev\npwd\nps\nstatus\nshutdown\n')
        reference, _ = self.guest('good', commands=commands)
        self.assertEqual(reference.returncode, 0, reference.stderr)
        for text in ('Fuzix 0.5 RCC-BF Ripple\n', '\nwritten-in-guest\nappended\n',
                     'cat\n', 'ls\n', 'uname\n', '/dev\n', 'shell pid 1\n', 'RBF shell shutdown.'):
            self.assertIn(text, reference.stdout)
        if 'rbf' not in BACKENDS: return
        result, report = self.guest('good', 'rbf', commands)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, reference.stdout)
        self.assertEqual(report['stop_reason'], 'halted')
        self.assertEqual(report['layout']['memory_banks'], 8)
        self.assertGreater(report['jit']['native_dispatches'], 0)
        self.assertEqual(report['tick'], 0)

    def test_syscall_bounds_and_files_on_both_engines(self):
        for backend in BACKENDS:
            result, _ = self.guest('abi', backend)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('ABI checks: ' + 'Y' * 20 + '\nABI OK\n', result.stdout)
            self.assertNotIn('ABI FAILED', result.stdout)

    def test_real_child_pids_fork_memory_exec_arguments_and_wait(self):
        for backend in BACKENDS:
            result, _ = self.guest('process', backend)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Process checks: YYYYY[child argv OK]YYYY\nProcess OK\n', result.stdout)

    def test_writes_truncation_append_and_c16_words(self):
        for backend in BACKENDS:
            result, _ = self.guest('write', backend)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Write checks: YYYYYY\nWrite OK\n', result.stdout)
            # This C16 word sequence cannot come from an octet-encoded SYS1
            # file. Dirty-buffer eviction has reached the real block driver.
            image = self.directory / f'write-{backend}.working.img'
            self.assertIn(bytes.fromhex('0012011202120312'), image.read_bytes())

    def test_main_return_value_reaches_the_host(self):
        for backend in BACKENDS:
            result, report = self.guest('exit', backend)
            self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
            self.assertIn('RBF shell shutdown.', result.stdout)
            self.assertEqual(report['stop_reason'], 'halted')

    def test_bad_executables_fail_before_starting_user_code(self):
        cases = {name: 'executable' for name in ('magic','bank','placement','gp','short','no-exec',
                    'entry','empty','huge-code','huge-data','truncated','trailing')}
        cases['missing'] = 'missing'
        for name, reason in cases.items():
            for backend in (BACKENDS if name in ('magic', 'huge-code') else ('rvm',)):
                with self.subTest(name=name, backend=backend):
                    result, _ = self.guest(name, backend)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(f'panic: init {reason}', result.stdout)
                    self.assertNotIn('Starting /init', result.stdout)

    def test_code_upload_crosses_bank_and_accepts_full_data_bank(self):
        result, report = self.guest('cross-bank')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Starting /init', result.stdout)
        self.assertEqual(report['stop_reason'], 'halted')

    def test_long_lines_arguments_editing_and_eof(self):
        commands = ('echo ' + 'x' * 700 + '\n' + 'echo ' + 'a ' * 24 + '\n'
                    'echo wrong\x15echo recovered\n' 'echo bax\x7fckspace\n'
                    'echo EOF line\x04shutdown\n')
        result, _ = self.guest('good', commands=commands)
        self.assertEqual(result.returncode, 0, result.stderr)
        for message in ('line too long (255 characters maximum)', 'too many arguments',
                        '\nrecovered\n', '\nbackspace\n', 'echo EOF lineEOF line\n', 'RBF shell shutdown.'):
            self.assertIn(message, result.stdout)
        result, _ = self.guest('good', commands='\x04')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RBF shell shutdown.', result.stdout)

    def test_child_reads_terminal_and_returns_to_parent(self):
        for backend in BACKENDS:
            result, _ = self.guest('good', backend,
                'cat\nchild-terminal\n\x04status\nps\nshutdown\n')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('child-terminal\nchild-terminal\n', result.stdout)
            self.assertIn('shell pid 1\n', result.stdout)
            self.assertIn('status\n0\n', result.stdout)
            self.assertIn('RBF shell shutdown.', result.stdout)

    def test_launcher_delivers_live_stdin_to_a_blocked_bf_read(self):
        if 'rbf' not in BACKENDS: self.skipTest('RVM-only run')
        report = self.directory / 'live.json'
        child = subprocess.Popen([str(ROOT / 'scripts/run-fuzix-bf.sh'), '--run-only',
            '--build', str(BUILD), '--no-telemetry', '--timeout', '180', '--report', str(report)],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        output = bytearray()
        def until(marker):
            deadline = time.monotonic() + 180
            while marker not in output:
                left = deadline - time.monotonic()
                self.assertGreater(left, 0, output.decode(errors='replace'))
                self.assertTrue(select.select([child.stdout], [], [], left)[0])
                chunk = os.read(child.stdout.fileno(), 65536)
                self.assertTrue(chunk, output.decode(errors='replace'))
                output.extend(chunk)
        try:
            until(b'fuzix-bf# ')
            child.stdin.write(b'echo delayed'); child.stdin.flush()
            until(b'echo delayed')
            self.assertIsNone(child.poll())
            self.assertEqual(output.count(b'fuzix-bf# '), 1)
            self.assertNotIn(b'\r\ndelayed\r\n', output)
            child.stdin.write(b'\nshutdown\n'); child.stdin.flush()
            tail, errors = child.communicate(timeout=180)
            output.extend(tail)
            self.assertEqual(child.returncode, 0, errors.decode())
            self.assertIn(b'\r\ndelayed\r\n', output)
            self.assertIn(b'RBF shell shutdown.', output)
            self.assertTrue(json.loads(report.read_text())['success'])
        finally:
            if child.poll() is None:
                child.terminate(); child.communicate(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=BUILD)
    parser.add_argument('--rvm-only', action='store_true', help='quick guest checks without BF runs')
    args, rest = parser.parse_known_args()
    if args.rvm_only: BACKENDS = ('rvm',)
    BUILD = args.build.resolve()
    unittest.main(argv=[__file__, *rest])
