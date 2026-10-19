#!/usr/bin/env python3
"""Portable backend execution, guest parity and bootstrap-generation checks.

All disks are private. Host rasm/rlink still assemble/link the emitted text.
"""
import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from test_hosted import Guest
from build_ripple import ensure_host

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
ASM=ROOT/'src/ripple-asm/target/release'
ENV=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RIPPLE_NET='off')
FIXTURES=['control','scalars','memory','strings','aggregates','varargs','relocations',
          'inline_switch','wide_unary','floating_unary','preprocessor','backend_integer']


def run(args,expected=0,**kwargs):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,env=ENV,timeout=kwargs.pop('timeout',180),**kwargs)
    assert p.returncode==expected,(args,p.returncode,p.stdout[-3000:],p.stderr[-6000:])
    return p


def object_file(assembly):
    obj=assembly.with_suffix('.pobj')
    run([ASM/'rasm','assemble',assembly,'-o',obj,'--bank-size','64000','--max-immediate','65535','--memory-offset','0'])
    return obj


def execute(assembly,expected=0):
    obj=object_file(assembly);binary=assembly.with_suffix('.bin')
    run([ASM/'rlink',ROOT/'runtime/crt0.pobj',ROOT/'runtime/libruntime.par',obj,
         '-f','binary','--bank-size','64000','-o',binary])
    return run([ROOT/'target/release/rvm',binary,'--headless','--net','off','--disk',assembly.with_suffix('.disk'),
                '--exit-code','--max-instructions','30000000','--timeout','30'],expected=expected)


def rvm_boot(directory,image,commands,logname):
    log=HERE/'build'/logname
    with log.open('w') as output:
        p=subprocess.run(list(map(str,[ROOT/'target/release/rvm',ROOT/'build/fuzix/bios.bin',
            '--disk',image,'--memory','16384000','--headless','--jit','--net','off',
            '--input','\n'.join([*commands,'shutdown'])+'\n','--timeout','600'])),
            stdout=output,stderr=subprocess.STDOUT,timeout=620,env=ENV)
    text=log.read_text().replace('\r\n','\n')
    assert p.returncode==0 and 'Ripple: disk synced; power off.' in text,text[-8000:]
    return text


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guests',choices=['none','fuse-x','fuzix','all'],default='all')
    p.add_argument('--skip-build',action='store_true')
    args=p.parse_args()
    backend=ensure_host()
    if not args.skip_build:
        for osname in ['fuse-x','fuzix']:
            if args.guests in [osname,'all']:run([sys.executable,HERE/'build_ripple.py','--os',osname])
        if args.guests in ['fuzix','all']:
            run([sys.executable,HERE/'build_ripple.py','--os','fuzix','--backend','portable','-o',HERE/'build/apps/rcc-ripple-self.rxe'])
    with tempfile.TemporaryDirectory(prefix='portable-ripple-') as temp:
        directory=Path(temp);irfiles=[]
        for name in FIXTURES:
            ir=directory/f'{name}.ir'
            run([sys.executable,HERE/'cc.py',HERE/f'tests/{name}.c','--emit-ir','-o',ir])
            irfiles.append(ir)
        for name in ['parallel_phi','backward_values']:
            ir=directory/f'{name}.ir';shutil.copy2(HERE/f'tests/{name}.ir',ir);irfiles.append(ir)
        references={}
        for ir in irfiles:
            assembly=ir.with_suffix('.host.asm');run([backend,'--unit',ir.stem,ir,'-o',assembly])
            expected=21 if ir.stem=='parallel_phi' else 28 if ir.stem=='backward_values' else 0
            result=execute(assembly,expected)
            if ir.stem=='strings':assert 'portable C11 frontend' in result.stdout,result
            references[ir.stem]=assembly.read_bytes()
        print(f'{len(irfiles)} portable-backend programs executed successfully',flush=True)
        # Diagnostics must not replace an existing output or leave staging files.
        rejected=[('module m {target native}', 'cannot compile target'),
            ('module m {function i16 @main(){block 0: ret %3}}','undefined SSA'),
            ('module m {function i16 @main(){block 0: %0 = alloca [32001 x i16] -> i16* ret 0}}','frame limit'),
            ('module m {function i16 @main(){block 0: br label 3}}','unknown branch'),
            ('module m {function i16 @main(){block 0: %0 = add i16 1, 2}}','lacks a terminator'),
            ('module m {function i16 @main(){block 0: %0 = intrinsic i16 @bogus() ret %0}}','unsupported intrinsic')]
        for source,message in rejected:
            bad=directory/'bad.ir';bad.write_text(source);output=directory/'keep.asm';output.write_text('keep')
            result=run([backend,bad,'-o',output],expected=1)
            assert message in result.stderr and output.read_text()=='keep',result
            assert not list(directory.glob('keep.asm.tmp.*'))
        # Equal contents in distinct units must have distinct private symbols.
        static='module m {internal function @hidden function i16 @hidden(){block 0: ret 1}}'
        for name in ['unit-a','unit-b']:
            ir=directory/f'{name}.ir';ir.write_text(static);run([backend,ir,'-o',ir.with_suffix('.asm')])
        a=(directory/'unit-a.asm').read_text();b=(directory/'unit-b.asm').read_text();assert a!=b
        print('diagnostics, atomic output and distinct unit namespaces passed',flush=True)
        if args.guests in ['fuse-x','all']:
            share=directory/'share';share.mkdir();scratch=directory/'cache';scratch.mkdir()
            shutil.copy2(HERE/'build/apps/rcc-ripple.fxe',share)
            shutil.copy2(HERE/'build/apps/rcc-c.fxe',share)
            for ir in irfiles:shutil.copy2(ir,share)
            image=directory/'hosted.img';run(['node',ROOT/'fuse-x/image.mjs','create',image])
            guest=Guest(ROOT/'fuse-x/build/fuse-x',image,share,scratch)
            try:
                guest.prompt()
                for ir in irfiles:
                    name=ir.stem
                    output=guest.command(f'/mnt/host/rcc-ripple.fxe --unit {name} /mnt/host/{name}.ir -o /mnt/host/{name}.asm',180)
                    generated=share/f'{name}.asm'
                    assert generated.exists() and generated.read_bytes()==references[name],(name,output)
                # C -> IR -> assembly all runs in the hosted guest.
                shutil.copy2(HERE/'tests/backend_integer.c',share)
                shutil.copytree(ROOT/'runtime/include',share/'include')
                output=guest.command('/mnt/host/rcc-c.fxe -I/mnt/host/include /mnt/host/backend_integer.c -o /mnt/host/raw.ir',180)
                assert (share/'raw.ir').read_bytes()==(directory/'backend_integer.ir').read_bytes(),output
                output=guest.command('/mnt/host/rcc-ripple.fxe --unit backend_integer /mnt/host/raw.ir -o /mnt/host/raw.asm',180)
                assert (share/'raw.asm').read_bytes()==references['backend_integer'],output
                guest.child.stdin.write(b'shutdown\n');guest.child.stdin.flush();tail,_=guest.child.communicate(timeout=20)
                assert guest.child.returncode==0 and b'filesystem synced; session stopped' in tail,tail
                assert not list(scratch.iterdir())
            finally:
                (HERE/'build/ripple-hosted-transcript.log').write_bytes(guest.transcript);guest.close()
            print(f'Fuse-X: {len(irfiles)} exact host assembly matches and raw C pipeline passed',flush=True)
        if args.guests in ['fuzix','all']:
            build=ROOT/'build/fuzix';install=directory/'rvm';install.mkdir()
            for name in ['init.rxe','sh.rxe','echo.rxe','FUZIX.SYS','bios.bin']:(install/name).symlink_to(build/name)
            for name,executable in [('rcc-ripple','rcc-ripple.rxe'),('rcc-self','rcc-ripple-self.rxe'),('rcc-c','rcc-c.rxe')]:
                (install/f'{name}.rxe').symlink_to(HERE/'build/apps'/executable)
            (install/'apps.json').write_text(json.dumps(['init','sh','echo','rcc-ripple','rcc-self','rcc-c']))
            files=[dict(guest='/'+ir.name,host=str(ir)) for ir in irfiles]
            files.append(dict(guest='/raw.c',host=str(HERE/'tests/backend_integer.c')))
            for path in (ROOT/'runtime/include').rglob('*'):
                if path.is_file():files.append(dict(guest='/include/'+str(path.relative_to(ROOT/'runtime/include')),host=str(path)))
            manifest=install/'files.json';manifest.write_text(json.dumps(files));image=install/'test.img'
            run(['node',HERE/'test_rvm_image.mjs','create',image,install,manifest])
            commands=[];outputs=[]
            for ir in irfiles:
                for tool in ['rcc-ripple','rcc-self']:
                    generated=ir.stem+('-self' if tool=='rcc-self' else '')+'.asm'
                    commands += [f'{tool} --unit {ir.stem} /{ir.name} -o /{generated}','echo BACKEND_STATUS:$?']
                    outputs.append(dict(guest='/'+generated,host=str(install/generated)))
            commands += ['rcc-c -I/include /raw.c -o /raw.ir','echo FRONTEND_STATUS:$?',
                         'rcc-self --unit backend_integer /raw.ir -o /raw.asm','echo RAW_STATUS:$?']
            outputs.append(dict(guest='/raw.asm',host=str(install/'raw.asm')))
            output=rvm_boot(install,image,commands,'ripple-rvm-transcript.log')
            assert output.count('\nBACKEND_STATUS:0\n')==len(irfiles)*2,output[-8000:]
            assert '\nFRONTEND_STATUS:0\n' in output and '\nRAW_STATUS:0\n' in output,output[-8000:]
            manifest.write_text(json.dumps(outputs));run(['node',HERE/'test_rvm_image.mjs','extract',image,install,manifest])
            for file in outputs:
                name=Path(file['host']).stem.replace('-self','')
                if name=='raw':name='backend_integer'
                actual=Path(file['host']).read_bytes()
                if actual!=references[name]:
                    mismatch=HERE/'build'/('mismatch-'+Path(file['host']).name)
                    mismatch.write_bytes(actual)
                    mismatch.with_suffix('.expected.asm').write_bytes(references[name])
                    difference=''.join(difflib.unified_diff(
                        references[name].decode().splitlines(True),actual.decode().splitlines(True),
                        fromfile='host',tofile=file['guest']))
                    raise AssertionError(f'{mismatch}:\n{difference[:6000]}')
            # Package the guest's assembly and execute it as a Unix process.
            make=['make','--no-print-directory','-s','-C',ROOT/'ports/fuzix',f'BUILD={build}']
            run([*make,'rfl-sdk']);sdk=json.loads(run([*make,'rfl-sdk-info']).stdout)
            assembly=install/'raw.asm';obj=object_file(assembly);app=install/'generated.rxe'
            run([ASM/'rlink',sdk['entry'],obj,*sdk['libraries'],'-f','rxe','--bank-size','64000','-o',app])
            (install/'apps.json').write_text(json.dumps(['init','sh','echo','generated']))
            manifest.write_text('[]');image2=install/'execution.img'
            run(['node',HERE/'test_rvm_image.mjs','create',image2,install,manifest])
            result=rvm_boot(install,image2,['generated','echo EXECUTION_STATUS:$?'],'ripple-rvm-execution.log')
            assert '\nEXECUTION_STATUS:0\n' in result,result
            print(f'Riplix/RVM: {len(irfiles)*2} exact host assembly matches across both backend builds; raw C pipeline and guest RXE execution passed',flush=True)
    print('portable Ripple backend checks passed',flush=True)

if __name__=='__main__':main()
