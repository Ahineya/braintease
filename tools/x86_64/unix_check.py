"""Run the shared Unix utility cases with freestanding apps and a disposable root.

The existing boot image is mounted with snapshot=on, firmware variables are
copied, and QEMU has no network or physical disks. No kernel rebuild is needed.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess
import sys
from boot import ROOT, firmware
from qemu_check import run_shell_check

class UnixGuest:
    prompt=b'fuse-x# '
    def __init__(self,work,boot,applications,files,count):
        self.work=Path(work);self.image=self.work/'root.img'
        boot=Path(boot).resolve()
        for path in (boot/'boot.img',applications/'manifest.json'):
            if not path.is_file():raise RuntimeError('Missing prerequisite: '+str(path))
        # Exercise the actual x64 image producer, including disabled scheduling.
        subprocess.run(['node',str(ROOT/'tools/x86_64/riplix/root.mjs'),str(self.image),
                        str(applications),'--offline','--root-mib','128'],cwd=ROOT,check=True)
        suite='for i in '+' '.join(map(str,range(count)))+'; do echo UNIX_CASE:$i; sh /tmp/case$i; done\necho UNIX_SUITE_DONE\n'
        files=[*files,dict(name='/tmp/unix-all',mode=0o100600,text=suite)]
        manifest=self.work/'fixtures.json';manifest.write_text(json.dumps(files))
        builder="""import fs from 'node:fs';import {openImage} from './ports/fuzix/tools/image.mjs';import {atomicWrite} from './ports/fuzix/tools/disk.mjs';import {installRoot} from './ports/fuzix/tools/install.mjs';const [image,list]=process.argv.slice(1);const files=JSON.parse(fs.readFileSync(list,'utf8')).map(f=>({...f,data:Buffer.from(f.text||'')}));atomicWrite(image,installRoot(openImage(image),files));"""
        subprocess.run(['node','--input-type=module','-e',builder,str(self.image),str(manifest)],cwd=ROOT,check=True)
        code,variables=firmware(argparse.Namespace(firmware_code=None,firmware_vars=None))
        shutil.copyfile(variables,self.work/'vars.fd');serial=self.work/'serial.log'
        command=['qemu-system-x86_64','-machine','q35','-accel','tcg','-cpu','qemu64','-m','512M','-smp','1','-no-reboot','-nic','none','-monitor','none',
            '-drive',f'if=pflash,format=raw,readonly=on,file={code}',
            '-drive',f'if=pflash,format=raw,file={self.work/"vars.fd"}',
            '-drive',f'format=raw,snapshot=on,file={boot/"boot.img"}',
            '-blockdev',json.dumps(dict(driver='raw',**{'node-name':'unix-root'},file=dict(driver='file',filename=str(self.image)))),
            '-device','virtio-blk-pci,drive=unix-root,disable-legacy=on,num-queues=1,queue-size=8,rerror=report,werror=report',
            '-serial',f'file:{serial}','-display','none']
        try:
            with (self.work/'qemu.log').open('w') as log:
                result=run_shell_check(command,serial,log,480,self.work/'screen.png',persistent=True,
                    debug_exit=False,checks_only=True,extra_checks=[('sh /tmp/unix-all',r'UNIX_SUITE_DONE')])
                if result.returncode:raise RuntimeError('QEMU exited unsuccessfully')
        finally:
            self.log=serial.read_bytes() if serial.exists() else b''
            (ROOT/'build/unix-tools/x64-tests.log').write_bytes(self.log)
        text=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',self.log.decode(errors='replace').replace('\r',''))
        chunks=re.split(r'(?m)^UNIX_CASE:(\d+)\n',text)
        self.results={int(chunks[i]):chunks[i+1].split('UNIX_SUITE_DONE',1)[0] for i in range(1,len(chunks),2)}
        if len(self.results)!=count:raise RuntimeError('Missing x64 test results; inspect build/unix-tools/x64-tests.log')
    def shell(self,command,seconds=180):
        index=int(command.rsplit('case',1)[1]);return command+'\n'+self.results[index]+self.prompt.decode()
    def close(self):
        sys.path.insert(0,str(ROOT/'fuse-x'))
        from test_support import check_root
        check_root(self.image)


def build_fixtures():
    import apps
    previous=apps.NAMES
    try:
        apps.NAMES=('sh','echo','cat','tree','getopt','cron','crond','at','atrun',
                    'cal','mkdir','chmod','sleep','ln','cp','pwd','svcd','svc','test')
        return apps.build_apps(ROOT/'build/unix-tools/x64','minimal')
    finally:
        apps.NAMES=previous

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['build'],help='build the focused private x64 acceptance fixture')
    parser.parse_args()
    build_fixtures()
