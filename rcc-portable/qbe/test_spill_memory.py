#!/usr/bin/env python3
"""Bound real QBE allocations on large call-heavy functions.

This independent host build keeps host C widths; it checks the spilling
algorithm's storage lifetime, not the C16 layout. x86 guest source builds
separately check the actual bounded allocator and generated program execution.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
from unittest.mock import patch

import adapt_qbe as adapter
from build import ensure_host
from test_parallel_moves import fixture as phi_fixture

COUNTERS = r'''
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#undef malloc
#undef calloc
#undef free
void *calloc(size_t, size_t);
void free(void *);
static size_t live, peak;
static int registered;
static void report(void) { fprintf(stderr, "peak:%zu\n", peak); }
void *qbe_test_calloc(size_t count, size_t size) {
    size_t n, *block;
    if (size && count > (size_t)-1 / size) return NULL;
    n = count * size;
    /* Keep the bound deterministic and include all function-arena storage. */
    if (n > 12 * 1024 * 1024 || live > 12 * 1024 * 1024 - n) {
        fprintf(stderr, "allocation limit: live:%zu request:%zu\n", live, n);
        return NULL;
    }
    block = calloc(1, n + sizeof(size_t));
    if (!block) return NULL;
    if (!registered) { atexit(report); registered = 1; }
    *block = n;
    live += n;
    if (live > peak) peak = live;
    return block + 1;
}
void *qbe_test_malloc(size_t n) { return qbe_test_calloc(1, n); }
void qbe_test_free(void *pointer) {
    size_t *block;
    if (!pointer) return;
    block = (size_t *)pointer - 1;
    live -= *block;
    free(block);
}
'''


def fixture(name, count, floating=False):
    kind = 'd' if floating else 'l'
    lines = [f'export function {kind} ${name}({kind} %x) {{', '@entry']
    for i in range(count):
        value = 'x' if not i else 'v' + str(i - 1)
        lines.append(f' %v{i} ={kind} call $identity_{kind}({kind} %{value})')
    return '\n'.join(lines + [f' ret %v{count - 1}', '}', ''])


def main():
    with tempfile.TemporaryDirectory(prefix='qbe-spill-memory-', dir='/tmp') as folder:
        work = Path(folder)
        with patch.object(adapter, 'widen_integers', lambda s: s), patch.object(adapter, 'format_calls', lambda s: s):
            for name in adapter.SOURCES + ['all.h', 'ops.h', 'amd64/all.h']:
                dest = work / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                source = adapter.adapt(name, (adapter.UPSTREAM / name).read_text())
                source = source.replace('qbe_qsort(', 'qsort(')
                dest.write_text(source.replace('#define K 11183273UL', '#define K 11183273U'))
        (work / 'config.h').write_text('#define Deftgt T_amd64_sysv\n')
        (work / 'guest.c').write_text((adapter.HERE / 'guest.c').read_text().replace(
            'extern long qbe_main(long, char **);', 'extern int qbe_main(int, char **);'))
        (work / 'counters.c').write_text(COUNTERS)
        command = ['cc', '-std=c99', '-O1', '-D_POSIX_C_SOURCE=200809L',
                   '-Dmalloc=qbe_test_malloc', '-Dcalloc=qbe_test_calloc', '-Dfree=qbe_test_free',
                   '-I', str(work), '-I', str(adapter.HERE)]
        if os.environ.get('SDKROOT'):
            command += ['-isysroot', os.environ['SDKROOT']]
        if os.environ.get('QBE_SANITIZE'):
            command += ['-g', '-fsanitize=' + os.environ['QBE_SANITIZE']]
        tool = work / 'qbe'
        subprocess.run(command + [str(work / n) for n in adapter.SOURCES]
                       + [str(work / 'guest.c'), str(work / 'counters.c'), '-o', str(tool)], check=True)
        source = work / 'calls.ssa'
        # Grow, shrink and grow again in one process: scratch pointers must be
        # reinitialized after freeall(), including the nested dopm -> limit2 use.
        source.write_text(fixture('large', 5000) + fixture('small', 2, True)
                          + fixture('large_fp', 5300, True) + fixture('tiny', 1))
        output = work / 'calls.s'
        output.write_text('preserved\n')
        result = subprocess.run([str(tool), '-t', 'amd64_sysv', '-o', str(output), str(source)],
                                capture_output=True, text=True,
                                timeout=300 if 'address' in os.environ.get('QBE_SANITIZE', '') else 60)
        assert result.returncode == 0, result.stderr
        peaks = [int(n) for n in re.findall(r'peak:(\d+)', result.stderr)]
        assert peaks and max(peaks) < 12 * 1024 * 1024, result.stderr
        reference = work / 'reference.s'
        subprocess.run([str(adapter.PORTABLE / 'build/qbe-host/qbe'), '-t', 'amd64_sysv',
                        '-o', str(reference), str(source)], check=True)
        assert output.read_bytes() == reference.read_bytes(), 'spilling changed assembly'
        assert not list(work.glob('*.tmp.*')), 'temporary output leaked'
        fixtures = sorted((adapter.UPSTREAM / 'test').glob('[!_]*.ssa'))
        for fixture_path in fixtures:
            for compiler, destination in [(tool, output),
                    (adapter.PORTABLE / 'build/qbe-host/qbe', reference)]:
                subprocess.run([str(compiler), '-t', 'amd64_sysv', '-o', str(destination),
                                str(fixture_path)], check=True, capture_output=True, timeout=30)
            assert output.read_bytes() == reference.read_bytes(), fixture_path.name + ': selector changed assembly'
        # Exercise the factored allocator together with the larger move list;
        # upstream's fixed-capacity reference cannot compile these phi joins.
        wide = ensure_host(True)
        source.write_text(phi_fixture('many', 128) + phi_fixture('few', 2) + phi_fixture('again', 192))
        for compiler, destination in [(tool, output), (wide, reference)]:
            subprocess.run([str(compiler), '-t', 'amd64_sysv', '-o', str(destination), str(source)],
                           check=True, capture_output=True, timeout=60)
        assert output.read_bytes() == reference.read_bytes(), 'factored allocator changed large phi joins'
        print(str(len(fixtures)) + ' upstream fixtures and large/small/large phi joins match after QBE phase factoring')
        print(f'Large integer/FP call chains and repeated functions pass; peak {max(peaks):,} host bytes; exact upstream assembly')


if __name__ == '__main__':
    main()
