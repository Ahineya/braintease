#!/usr/bin/env python3
"""Run the RXE package engine with an existing x64 boot build, never a user disk."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'fuse-x'))
from test_support import check_root
sys.path.insert(0,str(ROOT/'tools/x86_64'))
from qemu_check import run_shell_check
from boot import firmware

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--boot',type=Path,default=ROOT/'build/x86_64-boot/normal')
    ap.add_argument('--catalog',type=Path,help='also install an actual x64 catalog app')
    args=ap.parse_args(); boot=args.boot.resolve()
    for p in (boot/'boot.img',boot/'root.img',ROOT/'build/pkg/pkg-engine-x64.rxe'):
        if not p.is_file():raise SystemExit(f'Missing prerequisite: {p}')
    code,variables=firmware(argparse.Namespace(firmware_code=None,firmware_vars=None))
    with tempfile.TemporaryDirectory(prefix='pkg-x64-') as temporary:
        work=Path(temporary).resolve(); disk=work/'root.img'
        files=[dict(name='/bin/pkg',mode=0o100755,path=str(ROOT/'build/pkg/pkg-engine-x64.rxe')),
               dict(name='/hello.rpk',path=str(ROOT/'build/pkg/packages/pkghello.rpk')),
               dict(name='/index.v1',path=str(ROOT/'build/pkg/mini-index.v1')),
               dict(name='/index.sig',path=str(ROOT/'build/pkg/mini-index.sig')),
               dict(name='/repo.pub',path=str(ROOT/'tools/packages/repo/mini.pub'))]
        extra=[]
        if args.catalog:
            catalog=json.loads(args.catalog.read_text())
            packages={p['name']:p for p in catalog['packages']}
            seen=set()
            def append_package(name):
                if name in seen:return
                p=packages[name]
                for dep in p['depends']:append_package(dep.split('@')[0])
                seen.add(name)
                path='/'+str(len(seen))+'.rpk'
                files.append(dict(name=path,path=p['archive']))
                extra.append((f'pkg --adopt-seed install {path} && echo CATALOG_INSTALLED',r'\r?\nCATALOG_INSTALLED\r?\n'))
            append_package('seq')
            extra.append(('/bin/seq 1 3',r'\r?\n1\r?\n2\r?\n3\r?\n'))
            append_package('test')
            extra.append(('/usr/bin/[ -f /bin/test ] && echo BRACKET_FILE_OK',r'\r?\nBRACKET_FILE_OK\r?\n'))
            extra.append(('if /bin/[ 2 -gt 3 ]; then echo BAD; else echo BRACKET_FALSE_OK; fi',r'\r?\nBRACKET_FALSE_OK\r?\n'))
            files.extend([dict(name='/etc/pkg/repo.conf',text='url=http://unused.test\n'),
                dict(name='/etc/pkg/repo.pub',path=str(ROOT/'tools/packages/repo/mini.pub')),
                dict(name='/var/cache/pkg/current/index.v1',path=str(ROOT/'build/pkg/mini-index.v1')),
                dict(name='/var/cache/pkg/current/index.sig',path=str(ROOT/'build/pkg/mini-index.sig'))])
            extra.append(('pkg search seq','seq 0.'))
        builder="""import fs from 'node:fs';import {openImage,writeImage} from './ports/fuzix/tools/image.mjs';import {growRoot,installRoot} from './ports/fuzix/tools/install.mjs';const [seed,out,list]=process.argv.slice(1);const files=JSON.parse(list).map(x=>({...x,data:x.path?fs.readFileSync(x.path):Buffer.from(x.text)}));writeImage(out,installRoot(growRoot(openImage(seed),32768),files));"""
        subprocess.run(['node','--input-type=module','-e',builder,str(boot/'root.img'),str(disk),json.dumps(files)],cwd=ROOT,check=True)
        shutil.copy(variables,work/'vars.fd')
        serial=work/'serial.log'
        command=['qemu-system-x86_64','-machine','q35','-accel','tcg','-cpu','qemu64','-m','512M','-smp','1','-no-reboot','-nic','none','-monitor','none',
                 '-drive',f'if=pflash,format=raw,readonly=on,file={code}',
                 '-drive',f'if=pflash,format=raw,file={work/"vars.fd"}',
                 '-drive',f'format=raw,snapshot=on,file={boot/"boot.img"}',
                 '-blockdev',json.dumps(dict(driver='raw',**{'node-name':'pkg-root'},file=dict(driver='file',filename=str(disk)))),
                 '-device','virtio-blk-pci,drive=pkg-root,disable-legacy=on,num-queues=1,queue-size=8,rerror=report,werror=report',
                 '-serial',f'file:{serial}','-display','none']
        checks=[('pkg inspect /hello.rpk','pkghello 1'),('pkg install /hello.rpk && echo INSTALLED',r'\r?\nINSTALLED\r?\n'),('/usr/bin/pkghello','Hello from the Riplix package repository!'),
                ('pkg list','pkghello 1'),('pkg remove pkghello && echo REMOVED',r'\r?\nREMOVED\r?\n'),('pkg verify /index.v1 /index.sig /repo.pub && echo VERIFIED',r'\r?\nVERIFIED\r?\n'),('pkg clean && echo CLEANED',r'\r?\nCLEANED\r?\n')]
        checks.extend(extra)
        try:
            with (work/'qemu.log').open('w') as log:
                run_shell_check(command,serial,log,300,work/'screen.png',persistent=True,debug_exit=False,checks_only=True,extra_checks=checks)
        finally:
            if serial.exists():(ROOT/'build/pkg/x64-guest.log').write_bytes(serial.read_bytes())
        check_root(disk)
        print('x64: actual RXE inspection, install/run/remove and software signature verification passed')

if __name__=='__main__':main()
