#!/usr/bin/env python3
"""Boot the real disk/kernel with the selected RVM engine(s) and verify persistent writes."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=PORT/'build')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a = p.parse_args()
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']
engine_label = 'JIT and interpreter' if a.engine == 'both' else a.engine
build = a.build.resolve()
vm = ROOT/'target/release/rvm'


def run(image, commands, jit):
    cmd = [str(vm), str(build/'bios.bin'), '--disk', str(image), '--memory', '16384000', '--headless', '--input', commands, '--timeout', '120']
    if jit:
        cmd.append('--jit')
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=130)
    assert result.returncode == 0, result.stdout
    assert 'Ripple: disk synced; power off.' in result.stdout, result.stdout
    assert 'error ' not in result.stdout and 'FAILED' not in result.stdout and 'panic:' not in result.stdout, result.stdout
    return result.stdout


def clean(image):
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(image)], check=True)


with tempfile.TemporaryDirectory(prefix='ripple-fuzix-test-') as temporary:
    image = Path(temporary)/'fuzix.img'
    subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(image)], check=True)
    output = run(image, 'echo durable_one > /note\necho durable_two >> /note\nmkdir /work\ncp /note /work/copy\nmv /work/copy /work/moved\nhello coffee\nforkcheck --stress\npreemptcheck\niotest create\nsync\nshutdown\n', engines[0])
    assert 'argument: coffee' in output, output
    assert 'child value=99' in output and 'parent value=7 child exit=23' in output, output
    assert 'forkcheck: bank snapshots and user zero passed' in output, output
    assert output.index('child ran while parent computed') < output.index('parent computation finished'), output
    assert 'iotest created 77777 characters' in output, output
    clean(image)
    before = hashlib.sha256(image.read_bytes()).digest()
    subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(image)], check=True)
    assert hashlib.sha256(image.read_bytes()).digest() == before
    output = run(image, 'cat /note\ncat /work/moved\niotest\nrm /note\nrm /work/moved\nrmdir /work\n'+'hello\n'*12+'shutdown\n', engines[-1])
    assert output.count('durable_one') == 2 and output.count('durable_two') == 2, output
    assert 'iotest verified, sought, truncated and deleted' in output, output
    assert output.count('Hello from a separate RCC userspace executable!') == 12, output
    clean(image)
    if (build/'.mmu').exists() and (build/'.mmu').read_text().strip() == '1':
        for jit in engines:
            output = run(image, 'mmucheck\necho MMU_STATUS:$?\nshutdown\n', jit)
            for expected in ['200000-cell heap and COW isolation passed',
                             'ten simultaneous children passed',
                             'kernel and MMIO protection passed', 'MMU_STATUS:0']:
                assert expected in output, output
            clean(image)
    output = run(image, 'ls\nshutdown\n', engines[0])
    assert 'warning: mounting dirty' not in output, output
    clean(image)
    # An actual host timeout after guest sync leaves a dirty superblock,
    # without losing the committed file. Recover it, then boot again.
    interrupted = subprocess.run([str(vm), str(build/'bios.bin'), '--disk', str(image),
        '--memory', '16384000', *(['--jit'] if engines[0] else []), '--headless', '--input',
        'echo crash_safe > /crashnote\nsync\n', '--timeout', '10'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
    assert 'synced' in interrupted.stdout, interrupted.stdout
    assert 'disk synced; power off' not in interrupted.stdout, interrupted.stdout
    result = subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(image)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert result.returncode != 0 and 'not shut down cleanly' in result.stdout, result.stdout
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(image), '--repair'], check=True)
    clean(image)
    output = run(image, 'cat /crashnote\nrm /crashnote\nshutdown\n', engines[0])
    assert 'crash_safe' in output, output
    clean(image)
print('FUZIX integration passed: fork/exec, timer preemption, indirect I/O, persistence, cleanup, offline recovery; ' + engine_label)
