#!/usr/bin/env python3
"""Exercise the shared C bulk-I/O ABI in RVM and the real BF CPU, with both JITs.

Build the Rust tools first. This test uses only temporary disks and artifacts.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def command(args, **kwargs):
    result = subprocess.run([str(a) for a in args], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=90, **kwargs)
    assert result.returncode == 0, result.stdout
    return result.stdout

with tempfile.TemporaryDirectory(prefix='ripple-bulk-c-') as directory:
    out = Path(directory)
    rcc = ROOT/'target/release/rcc'
    rasm = ROOT/'src/ripple-asm/target/release/rasm'
    rlink = ROOT/'src/ripple-asm/target/release/rlink'
    objects = []
    for source in ['runtime/crt0.asm', 'c-test/fixtures/devices/bulk-call.asm',
                   'c-test/fixtures/devices/bulk.c', 'runtime/vm_bulk.c', 'runtime/mmio.c']:
        source = ROOT/source
        assembly = source
        if source.suffix == '.c':
            assembly = out/(source.stem+'.asm')
            command([rcc, 'compile', source, '-o', assembly, '--bank-size', '1024',
                     '-I'+str(ROOT/'runtime/include'), '-DBANK_SIZE=1024'])
        obj = out/(source.stem+'.pobj')
        command([rasm, 'assemble', assembly, '-o', obj, '--bank-size', '1024', '--max-immediate', '65535'])
        objects.append(obj)
    binary, bf = out/'test.bin', out/'test.bf'
    command([rlink, *objects, '-f', 'binary', '--bank-size', '1024', '-o', binary])
    command([rlink, *objects, '-f', 'bf', '--standalone', '--bf-host', '--bank-size', '1024',
             '--memory-banks', '8', '--code-banks', '14', '-o', bf])
    # Read only transport metadata, never the expanded BF program.
    with bf.open() as stream:
        header = stream.read(512).split('END_BFHOST', 1)[0]
    cells = re.search(r'TAPE_CELLS (\d+)', header).group(1)
    for backend in ['rvm', 'rbf']:
        for jit in [False, True]:
            disk = out/f'{backend}-{jit}.img'
            disk.write_bytes(b'')
            args = [ROOT/f'target/release/{backend}', binary if backend == 'rvm' else bf,
                    '--disk', disk, '--headless', '--timeout', '60']
            if backend == 'rvm': args += ['--memory', '8192']
            else: args += ['--max-tape-cells', cells]
            if jit: args += ['--jit']
            output = command(args)
            assert 'YYYYYYYY' in output and 'N' not in output, output
            print(f'{backend} jit={jit}: C bulk helpers passed')
