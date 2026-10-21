#!/usr/bin/env python3
"""Execute portable kernel startup/linkage against an independent C16 oracle."""
import argparse
import ctypes
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, default=HERE.parent / 'build/rcc-qbe')
    p.add_argument('--qbe', type=Path, default=HERE.parent / 'build/qbe-host/qbe')
    a = p.parse_args()
    reference = (ROOT / 'rcc-ir/src/target.rs').read_text().split('pub const NATIVE_SERVICES:')[1]
    assert re.findall(r'\{"(\w+)", (\d+)\}', (HERE / 'services-table.h').read_text()) == re.findall(r'\("(\w+)", (\d+)\)', reference)
    with tempfile.TemporaryDirectory(prefix='rcc-kernel-test-') as folder:
        work = Path(folder)
        sources = [
            '''static int duplicate=17;
            int shared=23; int *self=&shared;
            extern int private_value; extern int peer(void); extern int app(void);
            extern int putchar(int); extern long rcc_process_getpid(void);
            extern long rcc_process_wait(long,int*);
            extern int unused(void);
            static int abandoned[64000];
            int main(void) {int status=0;
                if (duplicate!=17 || shared!=23 || self!=&shared || peer()!=29 || app()!=31) return 1;
                if (private_value!=31 || rcc_process_getpid()!=7) return 2;
                if (rcc_process_wait(9,&status)!=9 || status!=42) return 3;
                if (putchar(89)!=89) return 4;
                return 0;
            }''',
            '''static int duplicate=29; int peer(void) {return duplicate;}''',
            '''static int duplicate=31; int private_value=31;
            int app(void) {return duplicate;}''']
        irs = []
        for i, source in enumerate(sources):
            c = work / f'u{i}.c'
            ir = work / f'u{i}.ir'
            c.write_text(source)
            subprocess.run(['python3', str(HERE.parent / 'cc.py'), str(c), '--target', 'native', '--emit-ir', '-o', str(ir)], check=True)
            irs.append(ir)
        ssa, assembly, library = [work / ('kernel' + s) for s in ['.ssa', '.s', '.dylib']]
        command = [str(a.tool.resolve()), '--kernel', str(irs[0]), '--kernel', str(irs[1]), str(irs[2])]
        subprocess.run(command + ['-o', str(ssa)], check=True)
        text = ssa.read_text()
        assert 'shared-cells=5 private-bank=1 private-cells=3' in text
        # The same logical ABI must survive private objects spanning banks.
        (work / 'u2.c').write_text('static int duplicate=31; int private_value=31; int big[64001]; '
                                  'int app(void) {big[64000]=37; return duplicate+big[64000]-37;}')
        subprocess.run(['python3', str(HERE.parent / 'cc.py'), str(work / 'u2.c'), '--target', 'native',
                        '--emit-ir', '-o', str(irs[2])], check=True)
        subprocess.run(command + ['-o', str(ssa)], check=True)
        text = ssa.read_text()
        assert 'shared-cells=5 private-bank=32 private-cells=64004' in text
        assert 'rcc_native_process_checkpoint' not in text and 'rcc_native_image_token' not in text
        assert 'rcc_native_ext_putchar' in text and 'abandoned' not in text and 'unused' not in text
        target = 'arm64_apple' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'amd64_sysv'
        subprocess.run([str(a.qbe.resolve()), '-t', target, '-o', str(assembly), str(ssa)], check=True)
        glue = (HERE / 'tests/runtime.c').read_text().replace('cells[6*64000]', 'cells[256*64000]').replace('p>>16>=6', 'p>>16>=256').replace('6*64000', '256*64000')
        glue = glue.replace('U rcc_native_process_request(', 'U original_request(')
        glue += r'''
static U registered[65536], arities[65536], shared_cells, private_cells, seen;
U rcc_native_process_memory(struct context *c) {(void)c;return 0;}
U rcc_native_kernel_globals(struct context *c,U n) {(void)c;shared_cells=n;return 0;}
U rcc_native_global_memory(struct context *c,U bank,U n) {(void)c;if(bank!=32)abort();private_cells=n;return 0;}
U rcc_native_errno(struct context *c,U p) {(void)c;(void)p;return 0;}
U rcc_native_register(struct context *c,U token,U address) {
 (void)c;U n=(uint32_t)token^0x80000000U;if(!n || n>=65536 || registered[n])abort();
 registered[n]=address;arities[n]=token>>32;return 0;
}
U rcc_native_ext_putchar(struct context *c,U x) {(void)c;if(x!=89)abort();seen++;return x;}
U rcc_native_process_request(struct context *c,U op,U a,U b,U result) {
 U value;if(op==5)value=7;
 else if(op==3 && a==9){rcc_native_store(c,b,42,1);value=9;}
 else abort();
 if(result)top(c)->slots[result-1]=value;return 0;
}
U rcc_native_process_start(struct context *c,U token,U argc,U argv,U hosted) {
 if(argc || argv || hosted!=1)abort();rcc_native_process_call(c,token,0,0);
 U steps=0;while(c->depth){if(++steps>1000)abort();U n=(uint32_t)top(c)->token^0x80000000U;
 if(!registered[n] || arities[n]>64)abort();((U(*)(struct context*))(uintptr_t)registered[n])(c);}
 return c->args[0];
}
extern U rcc_native_entry(struct context *);
U probe_run(void) {struct context *c=calloc(1,sizeof *c);if(!c)abort();c->stack=128001;
 U status=rcc_native_entry(c);if(shared_cells!=5 || private_cells!=64004 || seen!=1)abort();free(c);return status;}
'''
        driver = work / 'oracle.c'
        driver.write_text(glue)
        compile = ['cc', '-std=c99', '-O2', '-shared', '-fPIC', str(driver), str(assembly), '-o', str(library)]
        if platform.system() == 'Darwin':
            compile += ['-isysroot', os.environ.get('SDKROOT') or subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()]
        else:
            compile += ['-Wl,-Bsymbolic']
        subprocess.run(compile, check=True)
        loaded = ctypes.CDLL(str(library))
        loaded.probe_run.restype = ctypes.c_uint64
        assert loaded.probe_run() == 0
        negatives = {
            'duplicate': command + [str(irs[1])],
            'missing-main': [str(a.tool.resolve()), '--kernel', str(irs[1])],
            'mixed-modes': command + ['--relocatable'],
        }
        snippets = {
            'unknown-service': 'function external i16 @missing() {} function i16 @main() {block 0: %0 = call i16 func @missing() ret %0}',
            'wrong-service-arity': 'function external i16 @putchar() {} function i16 @main() {block 0: %0 = call i16 func @putchar() ret %0}',
            'shared-overflow': '@g = external [64000 x i16] function i16 @main() {block 0: %0 = load i16, global @g ret %0}',
            'undefined-global': '@g = declaration i16 function i16 @main() {block 0: %0 = load i16, global @g ret %0}',
            'external-callback': '@g = external i16* = func @putchar function external i16 @putchar(i16 %0) {} function i16 @main() {block 0: %0 = load i16*, global @g %1 = call i16 %0(1) ret %1}',
        }
        for name, body in snippets.items():
            ir = work / (name + '.ir')
            ir.write_text('module negative {target native ' + body + '}')
            negatives[name] = [str(a.tool.resolve()), '--kernel', str(ir)]
        # Cross-unit conflicting declarations must not choose an arbitrary ABI.
        for name, body in {
            'function-abi': 'function external f64 @peer() {}',
            'global-abi': '@shared = declaration f64',
            'symbol-kind': '@peer = external i16 = 1',
        }.items():
            ir = work / (name + '.ir')
            ir.write_text('module negative {target native ' + body + '}')
            negatives[name] = command + [str(ir)]
        for name, c in negatives.items():
            dest = work / 'preserved'
            dest.write_bytes(b'preserved')
            result = subprocess.run(c + ['-o', str(dest)], capture_output=True)
            assert result.returncode == 1 and result.stderr, (name, result)
            assert dest.read_bytes() == b'preserved' and not list(work.glob('preserved.tmp.*')), name
        print(f'Kernel startup, shared/private globals, static collisions, callbacks, native services, process requests and {len(negatives)} rejections passed')


if __name__ == '__main__':
    main()
