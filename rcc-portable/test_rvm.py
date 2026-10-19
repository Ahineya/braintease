#!/usr/bin/env python3
"""Run the portable C frontend as an RXE inside Riplix under RVM."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
from fuse_x import INCLUDES

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(command, **options):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **options)
    if result.returncode:
        raise RuntimeError(f'{command}:\n{result.stdout}\n{result.stderr}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--fixtures', nargs='+', choices=['scalars', 'strings', 'aggregates', 'varargs', 'relocations'],
                        default=['scalars', 'strings', 'aggregates', 'varargs', 'relocations'])
    args = parser.parse_args()
    build = args.build.resolve()
    executable = HERE / 'build/apps/rcc-c-metrics.rxe'
    if not args.skip_build:
        run([sys.executable, HERE / 'guest_build.py', '--os', 'fuzix', '--metrics',
             '--build', build, '-o', executable])
    binary = executable.read_bytes()
    assert binary[:4] in (b'RXE1', b'RXE2')
    entry, instructions, data_octets, relocations = struct.unpack_from('<IIII', binary, 8)
    print(f'compiler RXE: {instructions} instructions, {data_octets} data octets', flush=True)
    with tempfile.TemporaryDirectory(prefix='portable-rvm-') as temporary:
        directory = Path(temporary)
        for name in ['init.rxe', 'sh.rxe', 'echo.rxe', 'FUZIX.SYS', 'bios.bin']:
            (directory / name).symlink_to(build / name)
        (directory / 'rcc-c.rxe').symlink_to(executable)
        (directory / 'apps.json').write_text(json.dumps(['init', 'sh', 'echo', 'rcc-c']))
        inputs, outputs, commands = [], [], []
        for name in args.fixtures:
            source = HERE / f'tests/{name}.c'
            prepared = directory / f'{name}.i'
            command = [ROOT / 'target/release/rcpp', source, '--keep-line-directives',
                       '-D__STDC_VERSION__=201112L']
            for include in INCLUDES:
                command += ['-I', include]
            result = run(command)
            prepared.write_text('#line 1 ' + json.dumps(str(source)) + '\n' + result.stdout)
            inputs.append(dict(guest=f'/{name}.i', host=str(prepared)))
            for target in ['ripple', 'native']:
                host = directory / f'{name}.{target}.host.ir'
                run([HERE / 'build/rcc-c', '--target', target, prepared, '-o', host])
                guest = f'/{name}.{target}.ir'
                outputs.append(dict(guest=guest, host=str(directory / f'{name}.{target}.ir')))
                commands += [f'rcc-c --target {target} /{name}.i -o {guest}', 'echo COMPILE_STATUS:$?']
        # Install untouched C headers and raw source on this private disk.
        for path in (ROOT / 'runtime/include').rglob('*'):
            if path.is_file():
                inputs.append(dict(guest='/include/' + str(path.relative_to(ROOT / 'runtime/include')), host=str(path)))
        for name in ['preprocessor.c', 'preprocessor.h']:
            inputs.append(dict(guest='/' + name, host=str(HERE / 'tests' / name)))
        for target in ['ripple', 'native']:
            host = directory / f'raw.{target}.host.ir'
            run([HERE / 'build/rcc-c', '--target', target, '-I', ROOT / 'runtime/include',
                 HERE / 'tests/preprocessor.c', '-o', host])
            outputs.append(dict(guest=f'/raw.{target}.ir', host=str(directory / f'raw.{target}.ir')))
            commands += [f'rcc-c --target {target} -I/include /preprocessor.c -o /raw.{target}.ir', 'echo COMPILE_STATUS:$?']
        commands += ['rcc-c -E -I/include /preprocessor.c -o /raw.i', 'echo PREPROCESS_STATUS:$?',
                     'rcc-c --target native /raw.i -o /roundtrip.ir', 'echo COMPILE_STATUS:$?']
        outputs.append(dict(guest='/roundtrip.ir', host=str(directory / 'roundtrip.ir')))
        (directory / 'roundtrip.host.ir').write_bytes((directory / 'raw.native.host.ir').read_bytes())
        bad = directory / 'invalid.i'
        bad.write_text('int main(void){return missing;}\n')
        inputs.append(dict(guest='/invalid.i', host=str(bad)))
        commands += ['rcc-c /invalid.i -o /invalid.ir', 'echo INVALID_STATUS:$?', 'shutdown']
        manifest = directory / 'files.json'
        manifest.write_text(json.dumps(inputs))
        image = directory / 'test.img'
        run(['node', HERE / 'test_rvm_image.mjs', 'create', image, directory, manifest])
        before = time.monotonic()
        transcript = HERE / 'build/rvm-transcript.log'
        with transcript.open('w') as log:
            result = subprocess.run(list(map(str, [
                ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                '--memory', '16384000', '--headless', '--jit', '--net', 'off',
                '--input', '\n'.join(commands) + '\n', '--timeout', '600'
            ])), stdout=log, stderr=subprocess.STDOUT, timeout=620)
        output = transcript.read_text().replace('\r\n', '\n')
        assert result.returncode == 0, output
        assert output.count('\nCOMPILE_STATUS:0\n') == len(outputs), output
        assert '\nPREPROCESS_STATUS:0\n' in output, output
        assert '\nINVALID_STATUS:1\n' in output and 'undeclared identifier: missing' in output, output
        assert 'Ripple: disk synced; power off.' in output, output
        manifest.write_text(json.dumps(outputs))
        run(['node', HERE / 'test_rvm_image.mjs', 'extract', image, directory, manifest])
        for file in outputs:
            emitted = Path(file['host'])
            reference = emitted.with_name(emitted.stem + '.host.ir')
            if emitted.read_bytes() != reference.read_bytes():
                (HERE / 'build/rvm-mismatch.ir').write_bytes(emitted.read_bytes())
                (HERE / 'build/rvm-reference.ir').write_bytes(reference.read_bytes())
                raise AssertionError(f'{file["guest"]}: see build/rvm-mismatch.ir and build/rvm-reference.ir')
            # Execute native output of the RVM-resident frontend through the
            # host backend too; exact Ripple IR parity is checked above.
            if '.native.' in emitted.name:
                program = emitted.with_suffix('.program')
                run([ROOT / 'target/release/rcc', 'link-native', emitted, '-o', program])
                run([program], env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off'))
        peaks = list(map(int, re.findall(r'heap peak (\d+) cells', output)))
        report = dict(instructions=instructions, data_octets=data_octets, relocations=relocations,
                      compilations=len(outputs), seconds=round(time.monotonic()-before, 3),
                      max_heap_peak_cells=max(peaks))
        (HERE / 'build/rvm-report.json').write_text(json.dumps(report, indent=2) + '\n')
        for name in ['rvm-mismatch.ir', 'rvm-reference.ir']:
            (HERE / 'build' / name).unlink(missing_ok=True)
    print('Riplix/RVM guest compiler: both IR targets, executable output and diagnostics passed', flush=True)


if __name__ == '__main__':
    main()
