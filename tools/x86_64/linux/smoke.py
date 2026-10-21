#!/usr/bin/env python3
"""Boot x86 Linux hosted Riplix and exercise its real shell on a private disk."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('this acceptance check must execute on x86_64 Linux')
    build = args.build.resolve()
    executable = build / 'fuse-x'
    # Check the artifact too: the host architecture alone is insufficient.
    with executable.open('rb') as file:
        header = file.read(20)
    if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
        raise RuntimeError('hosted kernel must be a little-endian x86_64 ELF executable')
    environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                       RCC_NATIVE_AUDIO='off', RCC_NATIVE_TIMEOUT='120', RIPPLE_IMAGE_BACKUPS='0')
    with tempfile.TemporaryDirectory(prefix='riplix-x86-linux-', dir=build.parent) as temporary:
        directory = Path(temporary)
        image = directory / 'ROOTFS.IMG'
        # Named application builds need not produce the full install inventory.
        # Stage exactly this test's packages so image.mjs cannot fall back to
        # its older five-program default or install unrelated cached programs.
        staged = directory / 'apps'
        staged.mkdir()
        programs = ['native-hello', 'sh', 'cat', 'ls', 'cp', 'echo', 'sleep', 'ps']
        for name in programs:
            package = build / 'apps' / f'{name}.fxe'
            if not package.is_file():
                raise RuntimeError(f'missing acceptance executable: {package}')
            (staged / package.name).symlink_to(package)
        (directory / 'application-report.json').write_text(json.dumps(
            [dict(name=name, status='built') for name in programs]))
        subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                        '--apps', str(staged)], check=True, env=environment)
        script = directory / 'smoke.sh'
        script.write_text('echo x86-shell-ok\n'
                          'native-hello\n'
                          'echo persistent-x86-data > /tmp/x86-data\n'
                          'cp /tmp/x86-data /tmp/x86-copy\n'
                          'echo pipe-x86-ok | cat\n'
                          'cat /tmp/x86-copy\n'
                          'sleep 1 &\nwait\necho fork-wait-x86-ok\n'
                          'ps\necho x86-checks-complete\nshutdown\n')
        subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image),
                        '/tmp/x86-smoke', str(script)], check=True, env=environment)
        def run_shell(commands, log, timeout):
            try:
                result = subprocess.run([str(executable), '--run', str(image), '--one-shot'],
                                        input=commands, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, env=environment, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                (build.parent / log).write_bytes(error.stdout or b'')
                raise RuntimeError(f'x86_64 shell timed out; see {log}') from None
            output = result.stdout.decode(errors='replace').replace('\r', '')
            (build.parent / log).write_text(output)
            return result.returncode, output

        status, output = run_shell(b'sh /tmp/x86-smoke\n', 'shell.log', 150)
        print(output, end='')
        expected = ['x86-shell-ok', 'Independent executable, PID ', 'pipe-x86-ok', 'persistent-x86-data',
                    'fork-wait-x86-ok', 'x86-checks-complete', 'session stopped']
        if status or any(marker not in output for marker in expected) or \
                any(marker in output.lower() for marker in ['panic:', 'check failed', 'not found']):
            raise RuntimeError('x86_64 hosted shell acceptance failed; see shell.log')
        # Restart the actual kernel and read the data through its filesystem.
        status, output = run_shell(b'cat /tmp/x86-copy\nshutdown\n', 'restart.log', 60)
        if status or '\npersistent-x86-data\n' not in output or 'session stopped' not in output:
            raise RuntimeError('x86_64 filesystem restart check failed; see restart.log')
    (build.parent / 'result.json').write_text(json.dumps(dict(
        status='passed', architecture='x86_64', os='linux',
        checks=['ELF architecture', 'init and Bourne shell', 'native exec',
                'pipes', 'fork/wait', 'filesystem persistence after restart']), indent=2) + '\n')
    print('PASS: x86_64 Linux Riplix shell, native exec, pipes, fork/wait and persistence')


if __name__ == '__main__':
    main()
