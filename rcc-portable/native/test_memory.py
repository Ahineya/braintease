#!/usr/bin/env python3
"""Bound compiler-owned allocations while lowering many continuation functions.

The C/rct fixture checks program semantics separately. This probe is the memory
regression: libc's internal allocations and process virtual mappings are not
part of the explicit compiler allocation budget.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WRAPPER = r'''
#undef malloc
#undef calloc
#undef realloc
#undef free
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
union aligned { long double number; void *pointer; uint64_t bits; };
struct allocation { size_t size; union aligned alignment; };
static size_t live, peak, limit;
static int initialized;
static void report(void) { fprintf(stderr,"compiler allocations: peak=%lu live=%lu limit=%lu\n",(unsigned long)peak,(unsigned long)live,(unsigned long)limit); }
static void init(void) { if(!initialized){const char *s=getenv("RCC_TEST_MEMORY");initialized=1;limit=s?strtoull(s,0,10):0;atexit(report);} }
void *probe_malloc(size_t n) {struct allocation *p;init();if(n>SIZE_MAX-sizeof *p || (limit && (n>limit || live>limit-n)))return NULL;p=malloc(sizeof *p+n);if(!p)return NULL;p->size=n;live+=n;if(live>peak)peak=live;return p+1;}
void probe_free(void *p) {if(p){struct allocation *a=(struct allocation*)p-1;live-=a->size;free(a);}}
void *probe_calloc(size_t n,size_t size) {void *p;if(size && n>SIZE_MAX/size)return NULL;p=probe_malloc(n*size);if(p)memset(p,0,n*size);return p;}
void *probe_realloc(void *p,size_t n) {struct allocation *a,*b;size_t old;if(!p)return probe_malloc(n);if(!n){probe_free(p);return NULL;}a=(struct allocation*)p-1;old=a->size;init();if(n>SIZE_MAX-sizeof *a || (limit && (n>limit || live-old>limit-n)))return NULL;b=realloc(a,sizeof *a+n);if(!b)return NULL;b->size=n;live=live-old+n;if(live>peak)peak=live;return b+1;}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--limit', type=int, default=8 * 1024 * 1024)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='rcc-native-memory-') as folder:
        work = Path(folder)
        ir = work / 'many.ir'
        source = ROOT / 'c-test/tests/bugs/portable_native_function_scratch.c'
        subprocess.run(['python3', str(HERE.parent / 'cc.py'), str(source), '--target', 'native',
                        '--emit-ir', '-o', str(ir)], check=True)
        wrapper = work / 'allocator.c'
        wrapper.write_text(WRAPPER)
        sources = [*sorted(HERE.glob('*.c')), HERE.parent / 'ripple/parse.c', HERE.parent / 'ripple/helpers.c', wrapper]
        tool = work / 'rcc-qbe'
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2',
                   '-Dmalloc=probe_malloc', '-Dcalloc=probe_calloc', '-Drealloc=probe_realloc', '-Dfree=probe_free',
                   *map(str, sources), '-o', str(tool)]
        if os.uname().sysname == 'Darwin':
            command += ['-isysroot', os.environ.get('SDKROOT') or subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()]
        subprocess.run(command, check=True)
        result = subprocess.run([str(tool), '--kernel', str(ir), '-o', str(work / 'many.ssa')],
                                env=dict(os.environ, RCC_TEST_MEMORY=str(a.limit)), capture_output=True, text=True)
        print(result.stderr, end='')
        assert result.returncode == 0, 'portable native lowering exceeded its compiler allocation budget'
        assert 'export function l $rcc_native_entry' in (work / 'many.ssa').read_text()
        assert re.search(r'compiler allocations: peak=\d+', result.stderr)
        print('Many-function native lowering fits the explicit allocation budget')


if __name__ == '__main__':
    main()
