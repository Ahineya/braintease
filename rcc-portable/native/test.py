#!/usr/bin/env python3
"""Execute QBE lowering against an independent scalar continuation oracle.

The real x86/kernel/assembler acceptance is qbe/test_x86.py. This small host
oracle forces checkpoint suspension, validates live reloads and catches bad SSA
without relying on the kernel's timer cadence.
"""
import argparse
import ctypes
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,default=HERE.parent/'build/rcc-qbe')
    p.add_argument('--qbe',type=Path,default=HERE.parent/'build/qbe-host/qbe')
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='rcc-qbe-test-') as folder:
        work=Path(folder)
        for name in ['scalar','floating','phi','arithmetic','casts','varargs','dead','precision','aggregate','conversions','bitfields','pointer_difference']:
            source=HERE/'tests'/(name+'.c');ir=work/(name+'.ir');ssa=work/(name+'.ssa');assembly=work/(name+'.s')
            if source.exists():subprocess.run(['python3',str(HERE.parent/'cc.py'),str(source),'--target','native','--emit-ir','-o',str(ir)],check=True)
            else:ir.write_bytes(source.with_suffix('.ir').read_bytes())
            subprocess.run([str(a.tool.resolve()),str(ir),'-o',str(ssa)],check=True)
            target='arm64_apple' if platform.system()=='Darwin' and platform.machine()=='arm64' else 'amd64_apple' if platform.system()=='Darwin' else 'amd64_sysv'
            subprocess.run([str(a.qbe.resolve()),'-t',target,'-o',str(assembly),str(ssa)],check=True)
            functions=[(int(t),symbol) for t,_,symbol in re.findall(r'^# rcc-function (\d+) (\d+) (\w+)$',ssa.read_text(),re.M)]
            glue=(HERE/'tests/runtime.c').read_text()
            glue+='\n'+''.join(f'extern U {symbol}(struct context *);\n' for _,symbol in functions)
            glue+='U probe_run(void) { struct context *c=calloc(1,sizeof *c); U steps=0,status; if(!c)abort(); c->stack=128001; rcc_native_process_call(c,0x80000001,1,0); while(c->depth) { if(++steps>1000000)abort(); switch(top(c)->token ^ 0x80000000U) {\n'
            glue+=''.join(f'case {token}: {symbol}(c); break;\n' for token,symbol in functions)
            glue+='default:abort(); }} status=c->args[0];free(c);return status;}\n'
            driver=work/(name+'.c');driver.write_text(glue);library=work/(name+'.dylib')
            command=['cc','-std=c99','-O2','-shared','-fPIC',str(driver),str(assembly),'-o',str(library)]
            if platform.system()=='Darwin':
                sdk=os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
                command+=['-isysroot',sdk]
            subprocess.run(command,check=True)
            compiled=ctypes.CDLL(str(library));compiled.probe_run.restype=ctypes.c_uint64
            status=compiled.probe_run();assert status==0,(name,status)
            compiled.probe_yields.restype=ctypes.c_uint64
            if name in ('scalar','phi','varargs','aggregate'):assert compiled.probe_yields()>0,'forced checkpoint was not reached'
            print(name+': execution and forced continuation reloads passed',flush=True)
        negatives={
            'entry-phi':'module m {target native function i16 @main() {block 0: %0 = phi i16 ret %0}}',
            'missing-target':'module m {function i16 @main() {block 0: ret 0}}',
            'integer-float':'module m {target native function i16 @main() {block 0: %0 = add f32 0, 0 ret 0}}',
            'target':'module m {target ripple function i16 @main() {block 0: ret 0}}',
            'undefined':'module m {target native function i16 @main() {block 0: ret %5}}',
            'dominance':'module m {target native function i16 @main() {block 0: br 1, label 1, label 2 block 1: %5 = add i16 1, 2 br label 2 block 2: ret %5}}',
            'duplicate':'module m {target native function i16 @main() {block 0: %1 = add i16 1, 1 %1 = add i16 2, 2 ret %1}}',
            'external':'module m {target native function external i16 @missing() {} function i16 @main() {block 0: %0 = call i16 func @missing() ret %0}}',
            'conversion':'module m {target native function i16 @main() {block 0: %0 = intrinsic i16 @"float.f64.to_si"(0) ret 0}}',
            'globals':'module m {target native @g = external i16 = 0 function i16 @main() {block 0: ret 0}}',
            'dead-unknown-target':'module m {target native function i16 @main() {block 0: ret 0 block 1: br label 99}}',
            'dead-duplicate':'module m {target native function i16 @main() {block 0: ret 0 block 1: ret 0 block 1: ret 0}}',
            'dead-invalid-phi':'module m {target native function i16 @main() {block 0: ret 0 block 1: %1 = phi i16 [0, label 0] ret %1}}',
            'va-start-fixed':'module m {target native function i16 @main() {block 0: intrinsic void @"va_start"(0) ret 0}}',
            'va-start-args':'module m {target native function i16 @main() {block 0: intrinsic void @"va_start"() ret 0}}',
            'va-arg-void':'module m {target native function i16 @main() {block 0: intrinsic void @"va_arg"(0) ret 0}}',
            'variadic-arity':'module m {target native function i16 @f(i16 %0, ...) {block 0: ret %0} function i16 @main() {block 0: %0 = call i16 func @f(1) ret %0}}',
        }
        for name,text in negatives.items():
            ir=work/'bad.ir';out=work/'keep.ssa';ir.write_text(text);out.write_bytes(b'preserve')
            result=subprocess.run([str(a.tool.resolve()),str(ir),'-o',str(out)],capture_output=True)
            assert result.returncode and result.stderr,(name,'accepted invalid/unsupported IR')
            assert out.read_bytes()==b'preserve' and not list(work.glob('keep.ssa.tmp.*')),name+' damaged output'
        print(str(len(negatives))+' rejection/output-preservation cases passed')


if __name__=='__main__':main()
