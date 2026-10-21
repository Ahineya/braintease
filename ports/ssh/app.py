#!/usr/bin/env python3
"""Explicitly build the SSH service; does not install or enable it."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import shutil
from build import ROOT, PORT, build


def ripple_link(args,objects,directory,output):
    sdk=(args.sdk or ROOT/'build/fuzix').resolve()
    make=['make','-s','--no-print-directory','-C',str(ROOT/'ports/fuzix'),
          f'BUILD={sdk}',f'RCC={args.compiler.resolve()}']
    subprocess.run([*make,f'-j{args.j}','rfl-sdk'],check=True)
    import json
    info=json.loads(subprocess.check_output([*make,'rfl-sdk-info'],text=True))
    flags=['--target','ripple','-DRIPPLE_MMU=1','-DNO_64BIT','-DWOLFSSL_USER_SETTINGS']
    for include in ([PORT/'config/client'] if args.app!='sshd' else [])+[PORT,PORT/'config',directory/'source',directory/'crypto',ROOT/'ports/tls',
                    ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',
                    ROOT/'runtime/include',ROOT/'ports/fuzix/ripple']:
        flags+=['-I',str(include)]
    entry=directory/'ripple/application.ir'
    result=subprocess.run([str(args.compiler.resolve()),'compile',str(args.source.resolve()),
                           *flags,'--emit-ir'],capture_output=True,env=dict(os.environ,RUST_LOG='error'))
    entry.with_suffix('.log').write_bytes(result.stderr)
    if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace'))
    entry.write_bytes(result.stdout)
    assembler=ROOT/'src/ripple-asm/target/release/rasm'
    def lower(ir):
        assembly,obj=ir.with_suffix('.asm'),ir.with_suffix('.pobj')
        if obj.exists() and obj.stat().st_mtime_ns>=max(p.stat().st_mtime_ns for p in (ir,args.compiler,assembler)):
            return obj
        with ir.with_suffix('.lower.log').open('w') as log:
            subprocess.run([str(args.compiler.resolve()),'generate-asm','-i',str(ir),'-o',str(assembly)],stdout=log,stderr=log,check=True)
            subprocess.run([str(assembler),'assemble',str(assembly),'-o',str(obj),'--bank-size','64000','--max-immediate','65535'],stdout=log,stderr=log,check=True)
        return obj
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=args.j) as pool:
        lowered=list(pool.map(lower,[entry,*objects]))
    subprocess.run([sys.executable,str(ROOT/'ports/fuzix/tools/shared-link.py'),'--build',str(sdk),
                    '--linker',str(assembler.with_name('rlink')),'--compiler',str(args.compiler.resolve()),'--',
                    info['entry'],*map(str,lowered),*info['libraries'],'-f','rxe','--bank-size','64000','-o',str(output)],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build/ssh')
    parser.add_argument('--sdk',type=Path)
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    parser.add_argument('--app',choices=('sshd','ssh','ssh-keygen','sftp','scp','sftp-mount','all'),default='sshd')
    parser.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    parser.add_argument('--source',type=Path)
    parser.add_argument('-o','--output',type=Path)
    parser.add_argument('-j',type=int,default=3)
    args=parser.parse_args();base=args.build.resolve();suffix='.fxe' if args.target=='native' else '.rxe'
    if args.app=='all':
        if args.source or args.output:parser.error('--app all cannot use --source or -o')
        for app in (('sshd','ssh-keygen','ssh','sftp-mount') if args.target=='native' else ('sshd','ssh-keygen','ssh')):
            command=[sys.executable,__file__,'--app',app,'--build',str(base),'--target',args.target,'--compiler',str(args.compiler),'-j',str(args.j)]
            if args.sdk:command+=['--sdk',str(args.sdk)]
            subprocess.run(command,check=True)
        for app in ('sftp','scp'):shutil.copyfile(base/('ssh'+suffix),base/(app+suffix))
        return
    if args.app=='sftp-mount' and args.target=='ripple':parser.error('the guest filesystem bridge currently requires Fuse-X or x64')
    args.source=args.source or PORT/('sshd.c' if args.app=='sshd' else 'keygen.c' if args.app=='ssh-keygen' else 'mount.c' if args.app=='sftp-mount' else 'client.c')
    directory=base/('client' if args.app!='sshd' else '')
    directory.mkdir(parents=True,exist_ok=True)
    output=(args.output or base/(args.app+suffix)).resolve();output.parent.mkdir(parents=True,exist_ok=True)
    with (directory/(args.target+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        objects=build(directory,args.compiler,args.target,args.j,client=args.app!='sshd')
        if args.target=='ripple':
            ripple_link(args,objects,directory,output)
            print(f'Built {output}');return
        command=[sys.executable,str(ROOT/'fuse-x/cc.py'),str(args.source.resolve()),'--build',str((args.sdk or ROOT/'fuse-x/build').resolve()),
                 '--compiler',str(args.compiler.resolve()),'--static','-DWOLFSSL_USER_SETTINGS','-o',str(output)]
        for include in ([PORT/'config/client'] if args.app!='sshd' else [])+[PORT,PORT/'config',directory/'source',directory/'crypto',ROOT/'ports/tls']:
            command+=['-I'+str(include)]
        for obj in objects:command+=['--object-ir',str(obj)]
        subprocess.run(command,check=True,env=dict(os.environ,RUST_LOG='error'))
    print(f'Built {output}')


if __name__=='__main__':main()
