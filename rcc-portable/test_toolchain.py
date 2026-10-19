#!/usr/bin/env python3
"""Compile, assemble, link and execute without host compiler services.

Private disks only. Host tools prepare the initial SDK and tool executables;
all compiler stages exercised in the guest run as guest processes.
"""
import argparse
from contextlib import nullcontext
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from test_hosted import Guest

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
ENV=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RIPPLE_NET='off')


def run(args,expected=0,**kwargs):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,env=kwargs.pop('env',ENV),timeout=kwargs.pop('timeout',300),**kwargs)
    assert p.returncode==expected,(args,p.returncode,p.stdout[-2000:],p.stderr[-6000:])
    return p


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guests',choices=['none','fuse-x','fuzix','all'],default='all')
    p.add_argument('--skip-build',action='store_true')
    p.add_argument('--installed',action='store_true',help='use compiler packages installed by the normal image builders, with default PATH/SDK')
    p.add_argument('--bootstrap',action='store_true',help='also rebuild assembler/linker sources inside FUZIX')
    p.add_argument('--bootstrap-hosted',action='store_true',help='rebuild assembler/linker in Fuse-X, then execute that RXE in FUZIX (requires --guests all)')
    p.add_argument('--workdir',type=Path,help='retain disposable test files here for diagnosis (must not exist)')
    args=p.parse_args()
    if args.bootstrap_hosted and args.guests!='all':p.error('--bootstrap-hosted requires --guests all')
    if not args.skip_build:
        run([sys.executable,HERE/'build.py'])
        run([sys.executable,HERE/'build_ripple.py'])
        run([sys.executable,HERE/'sdk.py','--float'],timeout=900)
        run([sys.executable,HERE/'build_tools.py'])
        for osname in ['fuse-x','fuzix']:
            if args.guests in [osname,'all']:
                run([sys.executable,HERE/'guest_build.py','--os',osname],timeout=900)
                backend_args=['--backend','portable','-o',HERE/'build/apps/rcc-ripple-self.rxe'] if osname=='fuzix' else []
                run([sys.executable,HERE/'build_ripple.py','--os',osname,*backend_args],timeout=900)
                run([sys.executable,HERE/'build_tools.py','--os',osname,*(['--backend','portable'] if osname=='fuzix' else [])],timeout=600)
    sdk=HERE/'build/sdk'
    if args.workdir:
        args.workdir=args.workdir.resolve()
        args.workdir.mkdir(parents=True)
    with (nullcontext(str(args.workdir)) if args.workdir else tempfile.TemporaryDirectory(prefix='portable-toolchain-')) as temp:
        directory=Path(temp)
        env=dict(ENV,RCC_SDK=str(sdk),RCC_TOOLS=str(HERE/'build'))
        binary=directory/'host.bin'
        run([HERE/'build/rcc-cc','--bare',HERE/'tests/toolchain.c','-o',binary],env=env)
        result=run([ROOT/'target/release/rvm',binary,'--headless','--net','off','--disk',directory/'host.disk','--exit-code','--timeout','30'])
        assert 'Compiled and linked entirely in the guest!' in result.stdout,result
        print('host portable compiler/assembler/linker driver execution passed',flush=True)
        if args.guests in ['fuse-x','all']:
            share=directory/'share';share.mkdir();scratch=directory/'scratch';scratch.mkdir()
            if not args.installed:
                shutil.copytree(sdk,share/'sdk')
                for name in ['rcc-c','rcc-ripple','rcc-as','rcc-ld','rcc-cc']:
                    shutil.copy2(HERE/f'build/apps/{name}.fxe',share/name)
            shutil.copy2(HERE/'tests/toolchain.c',share/'hello.c')
            image=directory/'hosted.img'
            run(['node',ROOT/'fuse-x/image.mjs','create',image,*(['--apps',ROOT/'fuse-x/build/apps'] if args.installed else [])])
            driver='cc' if args.installed else '/mnt/host/rcc-cc --sdk /mnt/host/sdk --tools /mnt/host'
            guest=Guest(ROOT/'fuse-x/build/fuse-x',image,share,scratch,
                        timeout=1800 if args.bootstrap_hosted else 600)
            try:
                guest.prompt()
                output=guest.command(driver+' /mnt/host/hello.c -o /mnt/host/hello',240)
                assert (share/'hello').exists(),output
                data=(share/'hello').read_bytes();assert data[:4]==b'RXE1'
                # Host tools inspect/execute a bare image built wholly in Fuse-X.
                output=guest.command(driver+' --bare /mnt/host/hello.c -o /mnt/host/hello.bin',240)
                result=run([ROOT/'target/release/rvm',share/'hello.bin','--headless','--net','off','--disk',directory/'hosted.disk','--exit-code','--timeout','30'])
                assert 'Compiled and linked entirely in the guest!' in result.stdout,result
                if args.bootstrap_hosted:
                    shutil.copytree(HERE/'asm',share/'src')
                    sources=' '.join('/mnt/host/src/'+name+'.c' for name in ['common','assemble','link','main'])
                    output=guest.command(driver+' -v '+sources+' -o /mnt/host/as2',1650)
                    assert (share/'as2').is_file(),output
                    shutil.copy2(share/'as2',directory/'guest-as.rxe')
                guest.child.stdin.write(b'shutdown\n');guest.child.stdin.flush();tail,_=guest.child.communicate(timeout=20)
                assert guest.child.returncode==0 and b'filesystem synced; session stopped' in tail,tail
                assert not list(scratch.iterdir())
            finally:
                (HERE/'build/toolchain-hosted-transcript.log').write_bytes(guest.transcript);guest.close()
            print('Fuse-X: raw C -> IR -> assembly -> object -> RXE/binary passed',flush=True)
            if args.bootstrap_hosted:print('Fuse-X: rebuilt assembler/linker from raw C sources entirely in the guest',flush=True)
        if args.guests in ['fuzix','all']:
            build=ROOT/'build/fuzix';install=directory/'rvm';install.mkdir()
            for name in ['init.rxe','sh.rxe','echo.rxe','FUZIX.SYS','bios.bin']:(install/name).symlink_to(build/name)
            apps=['init','sh','echo']
            if args.installed:
                (install/'compiler').symlink_to(build/'compiler',target_is_directory=True)
            else:
                for name,tool in [('cc','rcc-cc'),('rcc-c','rcc-c'),('rcc-ripple','rcc-ripple-self'),('rcc-as','rcc-as'),('rcc-ld','rcc-ld')]:
                    (install/f'{name}.rxe').symlink_to(HERE/f'build/apps/{tool}.rxe');apps.append(name)
            (install/'apps.json').write_text(json.dumps(apps))
            files=[dict(guest='/hello.c',host=str(HERE/'tests/toolchain.c')),
                   dict(guest='/null.c',host=str(HERE/'tests/sdk_null.c'))]
            if not args.installed:
                for path in sdk.rglob('*'):
                    if path.is_file() and not any(part.startswith('.sdk-') for part in path.parts):
                        files.append(dict(guest='/usr/lib/rcc/'+str(path.relative_to(sdk)),host=str(path)))
            commands=['cc -v /hello.c -o /hello','echo COMPILE_STATUS:$?','/hello','echo RUN_STATUS:$?',
                      'cc /null.c -o /null','echo NULL_COMPILE_STATUS:$?',
                      '/null','echo NULL_RUN_STATUS:$?']
            if args.bootstrap or args.bootstrap_hosted:
                rebuilt_tool='/bin/as2' if args.bootstrap_hosted else '/as2'
                if args.bootstrap_hosted:
                    (install/'as2.rxe').symlink_to(directory/'guest-as.rxe');apps.append('as2')
                else:
                    for path in sorted((HERE/'asm').glob('*.[ch]')):files.append(dict(guest='/src/'+path.name,host=str(path)))
                    commands+=['cc -v /src/common.c /src/assemble.c /src/link.c /src/main.c -o /as2','echo BOOTSTRAP_STATUS:$?']
                commands+=[
                    'rcc-c -I/usr/lib/rcc/include /hello.c -o /hello.ir',
                    'rcc-ripple --unit hello /hello.ir -o /hello.asm',
                    f'{rebuilt_tool} /hello.asm -o /hello2.rpo','echo ASSEMBLE_STATUS:$?',
                    f'{rebuilt_tool} link /usr/lib/rcc/entry.rpo /hello2.rpo /usr/lib/rcc/libc.rpa -o /hello2','echo LINK_STATUS:$?',
                    'chmod +x /hello2','/hello2','echo SECOND_RUN_STATUS:$?']
                # chmod is needed only for direct linker use (cc sets it itself).
                (install/'chmod.rxe').symlink_to(build/'chmod.rxe');apps.append('chmod');(install/'apps.json').write_text(json.dumps(apps))
            manifest=install/'files.json';manifest.write_text(json.dumps(files));image=install/'test.img'
            run(['node',HERE/'test_rvm_image.mjs','create',image,install,manifest],timeout=180)
            log=HERE/'build/toolchain-rvm-transcript.log'
            with log.open('w') as output:
                child=subprocess.run(list(map(str,[ROOT/'target/release/rvm',build/'bios.bin','--disk',image,
                    '--memory','16384000','--headless','--jit','--net','off','--input','\n'.join([*commands,'shutdown'])+'\n','--timeout','1800'])),stdout=output,stderr=subprocess.STDOUT,timeout=1820,env=ENV)
            text=log.read_text().replace('\r\n','\n')
            assert child.returncode==0 and 'Ripple: disk synced; power off.' in text,text[-8000:]
            markers=['COMPILE_STATUS:0','RUN_STATUS:0','NULL_COMPILE_STATUS:0','NULL_RUN_STATUS:0']
            if args.bootstrap and not args.bootstrap_hosted:markers+=['BOOTSTRAP_STATUS:0']
            if args.bootstrap or args.bootstrap_hosted:markers+=['ASSEMBLE_STATUS:0','LINK_STATUS:0','SECOND_RUN_STATUS:0']
            for marker in markers:
                assert '\n'+marker+'\n' in text,text[-12000:]
            assert 'Compiled and linked entirely in the guest!' in text,text[-12000:]
            if args.bootstrap_hosted:
                verified=HERE/'build/apps/rcc-as-guest.rxe'
                shutil.copy2(directory/'guest-as.rxe',verified)
                verified.chmod(0o755)
            print('FUZIX/RVM: complete guest-only compilation and Unix execution passed'+('; rebuilt assembler/linker executed successfully' if args.bootstrap or args.bootstrap_hosted else ''),flush=True)
    print('portable toolchain checks passed',flush=True)

if __name__=='__main__':main()
