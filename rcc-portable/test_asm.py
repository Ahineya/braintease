#!/usr/bin/env python3
"""Portable object/archive regressions and executable ABI coverage (private files)."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parent


def run(args,ok=True):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=120)
    assert (p.returncode==0)==ok,(args,p.returncode,p.stdout[-1000:],p.stderr[-3000:])
    return p


def main():
    run([sys.executable,HERE/'build_tools.py'])
    run([sys.executable,HERE/'build_ripple.py'])
    with tempfile.TemporaryDirectory(prefix='portable-asm-') as tmp:
        d=Path(tmp)
        def assemble(name,text):
            source=d/(name+'.asm');source.write_text(text)
            obj=d/(name+'.rpo');run([HERE/'build/rcc-as',source,'-o',obj]);return obj
        def link(objects,output='test.bin',ok=True,extra=()):
            return run([HERE/'build/rcc-ld','-f','binary','-o',d/output,*extra,*objects],ok)
        def execute(output='test.bin'):
            run([ROOT/'target/release/rvm',d/output,'--headless','--net','off','--disk',d/'disk','--exit-code','--timeout','30'])
        # Conditional expansion, chunk padding, cross-bank calls and returns.
        start=assemble('start','_start:\n LI T0, 1\n BEQ T0, R0, bad\n CALL far\n HALT\nbad:\n BRK\n')
        padding=assemble('padding','\n'.join([' NOP']*64000)+'\n')
        far=assemble('far','far:\n LI T1, 1\n BNE T0, T1, bad_far\n RET\nbad_far:\n BRK\n')
        link([start,padding,far]);execute()
        # Archives select needed members only and recurse through dependencies.
        # Preserve return address around the nested call.
        a=assemble('a','far:\n ADD S0, RA, R0\n ADD S1, RAB, R0\n CALL other\n ADD RA, S0, R0\n ADD RAB, S1, R0\n RET\n')
        b=assemble('b','other:\n RET\n')
        unused=assemble('unused','far:\n CALL missing\n RET\n')
        archive=d/'lib.rpa';run([HERE/'build/rcc-ld','--archive','-o',archive,a,b,unused])
        link([start,archive]);execute()
        # Private labels may share a suffix across compilation-unit prefixes.
        first='__rcp_01234567_89abcdef_same'
        second='__rcp_01234567_89abcdee_same'
        compact=assemble('private-prefixes',f'''_start:
 CALL {first}
 CALL {second}
 LI T2, 1
 BNE T0, T2, bad
 LI T2, 2
 BNE T1, T2, bad
 HALT
bad:
 BRK
{first}:
 LI T0, 1
 RET
{second}:
 LI T1, 2
 RET
''')
        link([compact]);execute()
        unresolved=assemble('private-unresolved',f'_start:\n CALL {first}\n HALT\n')
        assert first in link([unresolved],'unresolved.bin',False).stderr
        # The scan buffer grows for long data lines and ignores large metadata
        # comments, including an unterminated final comment at EOF.
        cells=bytes(i%256 for i in range(600))
        data=assemble('long-line','.data\npayload:\n.word '+','.join(map(str,cells))+
                      '\n.code\n_start:\n HALT\n;'+('metadata'*12000))
        assert data.read_bytes()[-len(cells):]==cells
        sentinel=d/'preserved';sentinel.write_bytes(b'keep me')
        for objects,message in [([start],"undefined symbol 'far'"),([start,far,far],"duplicate symbol 'far'")]:
            result=link(objects,'preserved',False);assert message in result.stderr,result
            assert sentinel.read_bytes()==b'keep me'
        malformed=d/'bad.rpo';malformed.write_bytes(b'RPO1'+struct.pack('<6I',1,256,0,0,0,1))
        assert 'truncated object payload' in link([malformed],'preserved',False).stderr
        assert sentinel.read_bytes()==b'keep me'
        source=d/'invalid.asm';source.write_text('_start:\n BEQ R0, R0, 2\n')
        result=run([HERE/'build/rcc-as',source,'-o',sentinel],False)
        assert 'numeric branch displacements' in result.stderr and sentinel.read_bytes()==b'keep me'
        assert not list(d.glob('*.tmp.*'))
        print('chunk/bank crossings, lazy archive dependencies and atomic failure checks passed',flush=True)
        for name in ['control','scalars','memory','strings','aggregates','varargs','relocations','wide_unary','floating_unary','preprocessor','backend_integer','toolchain']:
            source=HERE/f'tests/{name}.c'
            env=dict(os.environ,RCC_SDK=str(HERE/'build/sdk'),RCC_TOOLS=str(HERE/'build'))
            p=subprocess.run([str(HERE/'build/rcc-cc'),'--bare',str(source),'-o',str(d/'test.bin')],capture_output=True,text=True,env=env,timeout=120)
            assert p.returncode==0,(name,p.stderr)
            execute();print(name+' execution passed',flush=True)
    print('portable assembler/linker checks passed',flush=True)

if __name__=='__main__':main()
