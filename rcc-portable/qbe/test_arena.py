#!/usr/bin/env python3
"""Check the QBE function allocator's zeroing, lifetime and overflow contract."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t live, requests;
static void *emalloc(size_t n) {
    void *p=calloc(1,n); if(!p)exit(99);live++;requests++;return p;
}
static void release(void *p) {if(!live)exit(98);live--;free(p);}
static void die(const char *s) {fprintf(stderr,"%s\n",s);exit(1);}
#define free release
#include "arena.inc"
#undef free
int main(int argc,char **argv) {
    unsigned char *p[2000],*large;
    size_t i,j;
    (void)argv;
    if(argc>1){alloc((size_t)-1);return 97;}
    if(alloc(0))return 1;
    for(i=0;i<2000;i++) {
        if(i==100) {
            large=alloc(20000);
            for(j=0;j<20000;j++)if(large[j])return 2;
            memset(large,0x5a,20000);
        }
        p[i]=alloc(5);
        if((uintptr_t)p[i]%_Alignof(QbeAlignment))return 3;
        for(j=0;j<5;j++){if(p[i][j])return 4;p[i][j]=(unsigned char)(i+j);}
    }
    for(i=0;i<2000;i++)for(j=0;j<5;j++)if(p[i][j]!=(unsigned char)(i+j))return 5;
    for(j=0;j<20000;j++)if(large[j]!=0x5a)return 6;
    if(requests>8)return 7;
    freeall();if(live)return 8;
    for(i=0;i<3;i++){unsigned char *q=alloc(9000);for(j=0;j<9000;j++)if(q[j])return 9;}
    freeall();freeall();return live!=0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='qbe-arena-', dir='/tmp') as folder:
        source = Path(folder) / 'test.c'; source.write_text(SOURCE)
        tool = Path(folder) / 'test'
        command = ['cc', '-std=c11', '-O1', '-fsanitize=undefined', '-I', str(HERE)]
        if os.environ.get('SDKROOT'):
            command += ['-isysroot', os.environ['SDKROOT']]
        subprocess.run(command + [str(source), '-o', str(tool)], check=True)
        subprocess.run([str(tool)], check=True)
        result = subprocess.run([str(tool), 'overflow'], capture_output=True, text=True)
        assert result.returncode == 1 and 'allocation span overflow' in result.stderr
    print('QBE arena preserves zeroing, alignment, stable disjoint objects and complete release; overflow rejects')


if __name__ == '__main__':
    main()
