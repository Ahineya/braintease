#!/usr/bin/env python3
"""Execute scratch retries/output preservation with an independent host build.

Keep host C integer widths for this oracle; actual C16 execution is validated
separately. Eight initial slots deliberately exhaust both parser and lowering.
"""
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

import adapt_qbe as adapter


def main():
    with tempfile.TemporaryDirectory(prefix='qbe-scratch-', dir='/tmp') as folder:
        work = Path(folder)
        with patch.object(adapter, 'widen_integers', lambda s: s), patch.object(adapter, 'format_calls', lambda s: s):
            for name in adapter.SOURCES + ['all.h', 'ops.h', 'amd64/all.h']:
                dest = work / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                text = adapter.adapt(name, (adapter.UPSTREAM / name).read_text()).replace('qbe_qsort(', 'qsort(')
                # C16 unsigned long is 32 bits; keep the lexer hash product
                # modulo 2^32 when this oracle uses host LP64 C instead.
                dest.write_text(text.replace('#define K 11183273UL', '#define K 11183273U'))
        (work / 'config.h').write_text('#define Deftgt T_amd64_sysv\n')
        support = work / 'guest.c'
        support.write_text((adapter.HERE / 'guest.c').read_text().replace(
            'extern long qbe_main(long, char **);', 'extern int qbe_main(int, char **);'))
        command = ['cc', '-std=c99', '-O1', '-D_POSIX_C_SOURCE=200809L', '-DQBE_INITIAL_SCRATCH=8',
                   '-I', str(work), '-I', str(adapter.HERE)]
        if os.environ.get('QBE_SANITIZE'):
            command += ['-g', '-fsanitize=' + os.environ['QBE_SANITIZE']]
        if os.environ.get('SDKROOT'):
            command += ['-isysroot', os.environ['SDKROOT']]
        tool = work / 'qbe'
        subprocess.run(command + [str(work / n) for n in adapter.SOURCES] + [str(support), '-o', str(tool)], check=True)
        checks = 0
        blit = work / 'expand.ssa'
        blit.write_text('export function $copy(l %a, l %b) {\n@start\n blit %a, %b, 1024\n ret\n}\n')
        for source in sorted((adapter.UPSTREAM / 'test').glob('[!_]*.ssa')) + [blit]:
            output = work / 'actual.s'
            reference = work / 'reference.s'
            result = subprocess.run([str(tool), '-t', 'amd64_sysv', '-o', str(output), str(source)], capture_output=True, timeout=60)
            if result.returncode:
                raise AssertionError(source.name + ': ' + result.stderr.decode())
            subprocess.run([str(adapter.PORTABLE / 'build/qbe-host/qbe'), '-t', 'amd64_sysv', '-o', str(reference), str(source)], check=True)
            # The adapted wrapper uses the guest heapsort. Equal-cost spills
            # can legitimately differ; execution of spill1 has its own oracle.
            if source.stem != 'spill1':
                assert output.read_bytes() == reference.read_bytes(), source.name
            checks += 1
        malformed = work / 'bad.ssa'
        malformed.write_text('function w $bad() {\n@start\n invalid\n}\n')
        output.write_text('preserved\n')
        result = subprocess.run([str(tool), '-o', str(output), str(malformed)], capture_output=True)
        assert result.returncode != 0 and output.read_text() == 'preserved\n'
        # Stdin/stdout cannot be replayed; neither debug output nor pipes use
        # retry children. Explicit -- and attached -o forms retain semantics.
        small = b'export function w $answer() {\n@start\n ret 42\n}\n'
        source = work / 'small.ssa'; source.write_bytes(small)
        for args, data in [(['-o' + str(output), '--', str(source)], None),
                           (['-o', str(output), '-'], small),
                           (['--'], small), (['-o', '-'], small),
                           (['-dP', '-o', str(output), str(source)], None),
                           (['-t?', '-o', str(output), str(source)], None),
                           (['-h', '-o', str(output), str(source)], None)]:
            result = subprocess.run([str(tool), *args], input=data, capture_output=True)
            assert result.returncode == 0, (args, result.stderr)
        assert not list(work.glob('*.tmp.*')), 'temporary output leaked'
        print(f'{checks} QBE fixtures pass forced scratch retries; failures/streams/options preserve output semantics')


if __name__ == '__main__':
    main()
