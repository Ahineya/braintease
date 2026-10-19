#!/usr/bin/env python3
"""Build applications after the kernel and exercise independent native exec."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    executable, compiler = args.executable.resolve(), args.compiler.resolve()
    here = Path(__file__).resolve().parent
    original = hashlib.sha256(executable.read_bytes()).digest()
    with tempfile.TemporaryDirectory(prefix='fuse-x-executables-') as temporary:
        directory = Path(temporary)
        scratch = directory / 'host-code'
        scratch.mkdir()
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                           RCC_NATIVE_TIMEOUT='30', TMPDIR=str(scratch))
        image = directory / 'root.img'
        subprocess.run(['node', str(here / 'image.mjs'), 'create', str(image),
                        '--bootstrap-shell', '--apps', str(executable.parent / 'apps')], check=True)

        def install(name, path):
            result = subprocess.run(['node', str(here / 'image.mjs'), 'put', str(image),
                                     '/bin/' + name, str(path)], capture_output=True, text=True)
            # This image is disposable. Retaining a full 1 GiB backup for every
            # malformed-package fixture can exhaust the host's temporary disk.
            for backup in directory.glob('root.img.bak*'):
                backup.unlink()
            assert result.returncode == 0, result.stdout + result.stderr

        def compile_app(source, output):
            result = subprocess.run(['python3', str(here / 'cc.py'), '--static', str(source), '-o', str(output),
                            '--compiler', str(compiler), '--build', str(executable.parent)],
                           capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr

        def run(commands):
            result = subprocess.run([str(executable), '--run', str(image)], input=commands,
                                    text=True, capture_output=True, env=environment, timeout=35)
            output = result.stdout + result.stderr
            assert result.returncode == 0 and 'filesystem synced; session stopped' in output, output
            assert 'panic:' not in output and 'CHECK FAILED' not in output, output
            assert not list(scratch.iterdir()), 'loaded native images leaked after shutdown'
            return output

        # Compile a new program after the host kernel exists; no host relinking.
        custom = directory / 'outside.c'
        custom.write_text('#include <stdio.h>\nint main(void) { puts("Built outside the kernel"); return 0; }\n')
        program = directory / 'outside.fxe'
        compile_app(custom, program)
        install('outside', program)
        check = directory / 'image-check.fxe'
        compile_app(here / 'examples/processes.c', check)
        install('image-check', check)
        global_check = directory / 'global-banks.fxe'
        compile_app(here / 'examples/global-banks.c', global_check)
        install('global-banks', global_check)
        large = directory / 'large-data'
        large.write_bytes(bytes(i % 251 for i in range(70000)))
        install('large-source', large)
        assert hashlib.sha256(executable.read_bytes()).digest() == original, 'application build changed kernel'

        package = program.read_bytes()
        json_size, code_size = struct.unpack('<II', package[8:16])
        manifest = json.loads(package[16:16 + json_size])
        payload = package[16 + json_size:]

        def bad(name, mutate):
            metadata = json.loads(json.dumps(manifest))
            mutate(metadata)
            encoded = json.dumps(metadata).encode()
            path = directory / (name + '.fxe')
            path.write_bytes(package[:8] + struct.pack('<II', len(encoded), code_size) + encoded + payload)
            install(name, path)

        bad('bad-abi', lambda m: m.update(abi=999))
        bad('bad-cpu', lambda m: m.update(arch='wrong-cpu'))
        bad('bad-entry', lambda m: m.update(entry=0))
        bad('bad-data', lambda m: m.update(initializers=[[0xf00001, 1, 1, False]]))
        bad('bad-global-bank', lambda m: m.update(global_bank=2))
        bad('bad-global-size', lambda m: m.update(global_cells=1024001))
        def legacy(m):
            m['abi'] = 1
            m.pop('global_bank')
        bad('legacy', legacy)
        bad('bad-helper', lambda m: m['helpers'].append({'symbol': 'rcc_native_ext_fopen', 'arity': 2}))
        truncated = directory / 'truncated.fxe'
        truncated.write_bytes(package[:-32])
        install('truncated', truncated)
        output = run('outside\nwrite /tmp/source "persistent native copy"\n'
                     'cp /tmp/source /tmp/copied\ncat /tmp/copied\nls -l /tmp\n'
                     'cp /bin/large-source /tmp/large-copy\n'
                     'cp /bin/outside /tmp/copied-app\n/tmp/copied-app\n'
                     'image-check\n/bin/global-banks\nlegacy\n' + ''.join(name + '\n' for name in
                     ['bad-cpu', 'bad-entry', 'bad-data', 'bad-global-bank', 'bad-global-size',
                      'bad-helper', 'truncated']) + 'shutdown\n')
        assert 'Built outside the kernel\n' in output, output
        assert 'persistent native copy\n' in output and 'copied\n' in output, output
        assert 'independent image checks passed' in output, output
        assert 'GLOBAL_BANKS_OK ' in output and 'GLOBAL_BANKS_EXEC_OK' in output, output
        assert 'GLOBAL_CHECK_FAILED' not in output, output
        assert output.count('executable rejected:') + output.count('library resolution failed:') == 8, output
        output = run('cat /tmp/copied\nshutdown\n')
        assert 'persistent native copy\n' in output, output

        # Retired images must disappear while the kernel is still running too.
        child = subprocess.Popen([str(executable), '--run', str(image)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        received = bytearray()

        def expect(marker):
            deadline = time.monotonic() + 10
            while marker not in received:
                assert time.monotonic() < deadline, received.decode(errors='replace')
                if select.select([child.stdout], [], [], .1)[0]:
                    chunk = os.read(child.stdout.fileno(), 8192)
                    assert chunk, received.decode(errors='replace')
                    received.extend(chunk)
            end = received.index(marker) + len(marker)
            del received[:end]

        try:
            expect(b'fuse-x# ')
            for _ in range(24):
                child.stdin.write(b'outside\n')
                child.stdin.flush()
                expect(b'Built outside the kernel\r\n')
                expect(b'fuse-x# ')
                assert not list(scratch.iterdir()), 'retired image survived process exit'
            child.stdin.write(b'shutdown\n')
            child.stdin.flush()
            child.communicate(timeout=10)
            assert child.returncode == 0
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()
        assert not list(scratch.iterdir())
    print('Fuse-X: independent compilation, real cat/ls/cp, image relocation/lifetime, '
          'fork/exec/COW, heap, invalid-image rollback and persistence checks passed')


if __name__ == '__main__':
    main()
