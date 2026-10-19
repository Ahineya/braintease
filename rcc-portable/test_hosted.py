#!/usr/bin/env python3
"""Run portable-frontend applications and the frontend itself inside Fuse-X."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from guest_build import FORMATTERS, support
from fuse_x import INCLUDES

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(command, **options):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **options)
    if result.returncode:
        raise RuntimeError(f'{command}:\n{result.stdout}\n{result.stderr}')
    return result


class Guest:
    def __init__(self, executable, image, share, scratch, timeout=600, trace=None):
        self.child = subprocess.Popen([str(executable), '--run', str(image), '--share', str(share), '--one-shot'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                                              RCC_NATIVE_TIMEOUT=str(timeout), TMPDIR=str(scratch)))
        self.buffer = bytearray()
        self.transcript = bytearray()
        self.trace = trace

    def prompt(self, timeout=60):
        marker = b'fuse-x# '
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            if time.monotonic() >= deadline:
                raise RuntimeError('guest timed out:\n' + self.transcript[-8000:].decode(errors='replace'))
            if select.select([self.child.stdout], [], [], .1)[0]:
                data = os.read(self.child.stdout.fileno(), 65536)
                if not data:
                    raise RuntimeError('guest exited:\n' + self.transcript[-8000:].decode(errors='replace'))
                self.buffer.extend(data)
                self.transcript.extend(data)
                if self.trace is not None:
                    self.trace.write(data)
                    self.trace.flush()
        end = self.buffer.index(marker) + len(marker)
        result = bytes(self.buffer[:end]).decode(errors='replace').replace('\r\n', '\n')
        del self.buffer[:end]
        return result

    def command(self, command, timeout=60):
        self.child.stdin.write((command + '\n').encode())
        self.child.stdin.flush()
        return self.prompt(timeout)

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.communicate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--apps', type=Path, default=HERE / 'build/apps')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    apps = args.apps.resolve()
    apps.mkdir(parents=True, exist_ok=True)
    if not args.skip_build:
        for name in ['cat', 'cmp', 'sort', 'grep', 'sum']:
            command = [sys.executable, HERE / 'fuse_x.py',
                       ROOT / f'ports/fuzix/upstream/Applications/util/{name}.c',
                       '--build', args.build, '-o', apps / f'{name}.fxe']
            if name == 'grep':
                command += ['-Dregerror=grep_regerror']
            run(command)
        run([sys.executable, HERE / 'fuse_x.py', HERE / 'tests/hosted_process.c',
             '--build', args.build, '-o', apps / 'processes.fxe'])
        run([sys.executable, HERE / 'guest_build.py', '--metrics', '--build', args.build,
             '-o', apps / 'rcc-c-metrics.fxe'])
        formatter = support(HERE / 'build/guest-support')
        command = [sys.executable, HERE / 'fuse_x.py', HERE / 'tests/guest_libc.c',
                   HERE / 'guest/strtoull.c', HERE / 'src/strtod.c', formatter, '-I', HERE / 'guest/include',
                   '-DRIPPLE_FLOAT_FORMAT', '--float-format', '--build', args.build,
                   '-o', apps / 'guest-libc.fxe']
        for name in FORMATTERS:
            command += ['-D', f'{name}=rcc_c_{name}']
        run(command)

    with tempfile.TemporaryDirectory(prefix='portable-hosted-') as temporary:
        directory = Path(temporary)
        share = directory / 'share'
        share.mkdir()
        scratch = directory / 'native-images'
        scratch.mkdir()
        for name in ['cat', 'cmp', 'sort', 'grep', 'sum', 'processes', 'rcc-c-metrics', 'guest-libc']:
            shutil.copy2(apps / f'{name}.fxe', share)
        image = directory / 'root.img'
        run(['node', ROOT / 'fuse-x/image.mjs', 'create', image])
        source = 'zebra\napple\nbanana\napple\n'
        (share / 'input.txt').write_text(source)
        (share / 'copy.txt').write_text(source)
        guest = Guest(args.executable.resolve(), image, share, scratch)
        report = []
        try:
            guest.prompt()
            output = guest.command('/mnt/host/cat.fxe /mnt/host/input.txt')
            assert source in output, output
            output = guest.command('/mnt/host/cmp.fxe /mnt/host/input.txt /mnt/host/copy.txt')
            assert 'Files are identical\n' in output, output
            (share / 'copy.txt').write_text('Z' + source[1:])
            output = guest.command('/mnt/host/cmp.fxe /mnt/host/input.txt /mnt/host/copy.txt')
            assert 'Files differ at byte position 0\n' in output, output
            guest.command('/mnt/host/sort.fxe -o /mnt/host/sorted.txt /mnt/host/input.txt')
            assert (share / 'sorted.txt').read_text() == 'apple\napple\nbanana\nzebra\n'
            output = guest.command('/mnt/host/grep.fxe -n apple /mnt/host/input.txt')
            assert '2:apple\n4:apple\n' in output, output
            output = guest.command('/mnt/host/sum.fxe /mnt/host/input.txt')
            assert '11835' in output, output
            for name, marker in [('processes', 'portable hosted process checks passed'),
                                 ('guest-libc', 'portable guest libc checks passed')]:
                output = guest.command(f'/mnt/host/{name}.fxe')
                assert marker in output, output
            print('five real utilities, shared-folder I/O, fork/exec and guest libc: passed', flush=True)

            # Raw C, headers and macro expansion now run wholly in the guest.
            for relative in ['runtime/include', 'ports/fuzix/ripple/include',
                             'ports/fuzix/upstream/Library/include',
                             'ports/fuzix/upstream/Kernel/include',
                             'rcc-portable/build/frontend', 'rcc-portable/guest/include']:
                shutil.copytree(ROOT / relative, share / 'sdk' / relative)
            shutil.copy2(ROOT / 'ports/fuzix/ripple/exec-args.h',
                         share / 'sdk/ports/fuzix/ripple/exec-args.h')
            for name in ['preprocessor.c', 'preprocessor.h']:
                shutil.copy2(HERE / 'tests' / name, share / name)
            raw = share / 'preprocessor.c'
            expected = run([HERE / 'build/rcc-c', '--target', 'native',
                            '-I', ROOT / 'runtime/include', raw]).stdout
            output = guest.command('/mnt/host/rcc-c-metrics.fxe --target native -I/mnt/host/sdk/runtime/include /mnt/host/preprocessor.c -o /mnt/host/raw.ir', 180)
            assert (share / 'raw.ir').read_text() == expected, output
            program = directory / 'raw.native'
            run([ROOT / 'target/release/rcc', 'link-native', share / 'raw.ir', '-o', program])
            run([program], env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off'))
            output = guest.command('/mnt/host/rcc-c-metrics.fxe -E -I/mnt/host/sdk/runtime/include /mnt/host/preprocessor.c -o /mnt/host/raw.i', 180)
            output = guest.command('/mnt/host/rcc-c-metrics.fxe --target native /mnt/host/raw.i -o /mnt/host/roundtrip.ir', 180)
            assert (share / 'roundtrip.ir').read_text() == expected, output
            # Give both hosts identical logical filenames for assert's __FILE__.
            unit = share / 'sdk/rcc-portable/build/frontend/expr.c'
            unit.write_text('#line 1 "portable/expr.c"\n' + unit.read_text())
            options = ['-I', share / 'sdk/rcc-portable/guest/include',
                       '-I', share / 'sdk/ports/fuzix/ripple/include',
                       '-I', share / 'sdk/ports/fuzix/upstream/Library/include',
                       '-I', share / 'sdk/runtime/include']
            expected = run([HERE / 'build/rcc-c', '--target', 'native', *options, unit]).stdout
            guest.command('cd /mnt/host/sdk')
            output = guest.command('/mnt/host/rcc-c-metrics.fxe --target native -Ircc-portable/guest/include -Iports/fuzix/ripple/include -Iports/fuzix/upstream/Library/include -Iruntime/include rcc-portable/build/frontend/expr.c -o /mnt/host/raw-expr.ir', 240)
            assert (share / 'raw-expr.ir').read_text() == expected, output
            guest.command('cd /')
            print('guest preprocessing: raw C execution, -E round trip and raw compiler source passed', flush=True)

            sources = [HERE / f'tests/{name}.c' for name in
                       ['scalars', 'control', 'memory', 'strings', 'aggregates', 'varargs', 'relocations']]
            sources.append(HERE / 'build/frontend/expr.c')
            for source in sources:
                name = source.stem
                prepared = share / f'{name}.i'
                command = [ROOT / 'target/release/rcpp', source, '--keep-line-directives',
                           '-D__STDC_VERSION__=201112L', '-I', HERE / 'build/frontend',
                           '-I', HERE / 'guest/include']
                for include in INCLUDES:
                    command += ['-I', include]
                result = run(command)
                prepared.write_text('#line 1 ' + json.dumps(str(source)) + '\n' + result.stdout)
                host = share / f'{name}.host.ir'
                run([HERE / 'build/rcc-c', '--target', 'native', prepared, '-o', host])
                before = time.monotonic()
                output = guest.command(f'/mnt/host/rcc-c-metrics.fxe --target native /mnt/host/{name}.i -o /mnt/host/{name}.ir', 180)
                emitted = share / f'{name}.ir'
                assert emitted.exists() and emitted.read_bytes() == host.read_bytes(), (name, output)
                match = re.search(r'heap peak (\d+) cells; allocations (\d+)', output)
                assert match, output
                record = dict(source=name, input_cells=prepared.stat().st_size, ir_octets=emitted.stat().st_size,
                              seconds=round(time.monotonic()-before, 3),
                              heap_peak_cells=int(match[1]), allocations=int(match[2]))
                report.append(record)
                if name != 'expr':
                    program = directory / f'{name}.native'
                    run([ROOT / 'target/release/rcc', 'link-native', emitted, '-o', program])
                    run([program], env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off'))
                print(f'guest frontend {name}: exact host IR match; {record["heap_peak_cells"]} live cells; {record["seconds"]}s', flush=True)

            # The same guest compiler can emit the Ripple target too.
            output = guest.command('/mnt/host/rcc-c-metrics.fxe --target ripple /mnt/host/scalars.i -o /mnt/host/ripple.ir')
            host = share / 'ripple.host.ir'
            run([HERE / 'build/rcc-c', '--target', 'ripple', share / 'scalars.i', '-o', host])
            assert (share / 'ripple.ir').read_bytes() == host.read_bytes(), output
            (share / 'invalid.i').write_text('int main(void){return missing;}\n')
            output = guest.command('/mnt/host/rcc-c-metrics.fxe /mnt/host/invalid.i -o /mnt/host/invalid.ir')
            assert 'undeclared identifier: missing' in output, output
            guest.child.stdin.write(b'shutdown\n')
            guest.child.stdin.flush()
            tail, _ = guest.child.communicate(timeout=15)
            assert guest.child.returncode == 0 and b'filesystem synced; session stopped' in tail, tail
            assert not list(scratch.iterdir()), 'loaded images survived shutdown'
        finally:
            (HERE / 'build/hosted-transcript.log').write_bytes(guest.transcript)
            guest.close()
        (HERE / 'build/hosted-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('guest frontend native/Ripple IR parity, executable output and error diagnostics: passed')


if __name__ == '__main__':
    main()
