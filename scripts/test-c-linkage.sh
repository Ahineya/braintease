#!/usr/bin/env bash
# Regression coverage for separate compilation and callbacks on the unchanged ISA.
# Build tools first (cargo build --release and the separate ripple-asm workspace).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export RCC="${RCC:-$ROOT/target/release/rcc}"
export RASM="${RASM:-$ROOT/src/ripple-asm/target/release/rasm}"
export RLINK="${RLINK:-$ROOT/src/ripple-asm/target/release/rlink}"
export RVM="${RVM:-$ROOT/target/release/rvm}"
export LINKAGE_ROOT="$ROOT"
python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(os.environ['LINKAGE_ROOT'])
rcc, rasm, rlink, rvm = [os.environ[name] for name in ('RCC', 'RASM', 'RLINK', 'RVM')]

def command(argv, success=True):
    result = subprocess.run([str(arg) for arg in argv], capture_output=True, text=True, timeout=30)
    if success and result.returncode:
        raise AssertionError(f"{' '.join(map(str, argv))}\n{result.stdout}\n{result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError(f"Expected diagnostic: {' '.join(map(str, argv))}")
    return result

with tempfile.TemporaryDirectory(prefix='rcc-linkage-') as directory:
    work = Path(directory)
    fixtures = root / 'c-test/fixtures/linkage'
    def compile_c(source, name, bank):
        asm = work / (name + '.asm')
        command([rcc, 'compile', source, '-o', asm, '--bank-size', bank])
        obj = work / (name + '.pobj')
        command([rasm, 'assemble', asm, '-o', obj, '--bank-size', bank, '--memory-offset', 0])
        return obj

    for bank in (64000, 1024):
        startup = work / 'crt0.pobj'
        command([rasm, 'assemble', root / 'runtime/crt0.asm', '-o', startup,
                 '--bank-size', bank, '--memory-offset', 0])
        objects = [compile_c(fixtures / (name + '.c'), name, bank)
                   for name in ('main', 'first', 'second')]
        padding = []
        if bank == 1024:
            # A complete code bank between startup and C guarantees code pointers
            # have nonzero banks, including external relocations and callback tables.
            pad_source = work / 'padding.asm'
            pad_source.write_text('code_padding:\n' + 'NOP\n' * bank)
            pad_object = work / 'padding.pobj'
            command([rasm, 'assemble', pad_source, '-o', pad_object, '--bank-size', bank])
            padding = [pad_object]
        for order in (objects, objects[::-1]):
            binary = work / 'test.bin'
            command([rlink, startup, *padding, *order, '-o', binary, '--bank-size', bank])
            command([rvm, binary, '--bank-size', bank, '--exit-code'])
        print(f'PASS: globals, externs, private symbols, callbacks and tables (bank size {bank}, both link orders)')

    too_large = work / 'too_large.c'
    too_large.write_text('int data[1025]; int main(void) { return 0; }\n')
    result = command([rcc, 'compile', too_large, '-o', work / 'too_large.asm', '--bank-size', 1024], False)
    assert 'exceeds one data bank' in result.stderr + result.stdout

    large = work / 'large.c'
    large.write_text('int data_one[700];\n')
    obj_one = compile_c(large, 'large_one', 1024)
    large.write_text('int data_two[700];\n')
    obj_two = compile_c(large, 'large_two', 1024)
    result = command([rlink, obj_one, obj_two, '-o', work / 'overflow.bin', '--bank-size', 1024], False)
    assert 'exceeds GP bank capacity' in result.stderr + result.stdout

    large_frame = work / 'large_frame.c'
    for local_count, bank in ((1000, 1024), (32750, 64000)):
        large_frame.write_text(f'int main(void) {{ int local[{local_count}]; return 0; }}\n')
        result = command([rcc, 'compile', large_frame, '-o', work / 'large_frame.asm', '--bank-size', bank], False)
        assert 'exceeds stack frame capacity' in result.stderr + result.stdout

    declaration = work / 'undefined.c'
    declaration.write_text('extern int missing; int main(void) { return missing; }\n')
    obj = compile_c(declaration, 'undefined', 1024)
    result = command([rlink, startup, obj, '-o', work / 'undefined.bin', '--bank-size', 1024], False)
    assert 'Undefined data label: missing' in result.stderr + result.stdout
    print('PASS: compiler/linker data and frame overflow, undefined extern diagnostics')
PY
