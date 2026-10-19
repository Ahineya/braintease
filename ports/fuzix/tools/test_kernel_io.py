#!/usr/bin/env python3
"""Kernel I/O acceptance and comparable measurements on disposable native/RVM disks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import tempfile
from bench_foundations import build_probe, run, Guest, check_disk
ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT/'ports/fuzix'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['fuse-x','fuzix'],default='fuse-x')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--executable',type=Path)
    p.add_argument('--probe',type=Path)
    p.add_argument('--benchmark-only',action='store_true')
    p.add_argument('--repetitions',type=int,default=1)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.repetitions<1:p.error('--repetitions must be positive')
    native=a.os=='fuse-x';build=(a.native_build if native else a.build).resolve()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    exe=(a.executable or (build/'fuse-x' if native else ROOT/'target/release/rvm')).resolve()
    probe=out/('iocheck.fxe' if native else 'iocheck.rxe')
    if a.probe:probe.write_bytes(a.probe.read_bytes())
    else:build_probe(build,native,probe,PORT/'tools/iocheck.c')
    readiness=out/('selectcheck.fxe' if native else 'selectcheck.rxe')
    links=out/('symlinkcheck.fxe' if native else 'symlinkcheck.rxe')
    if not a.benchmark_only:
        build_probe(build,native,readiness,PORT/'tools/selectcheck.c')
        build_probe(build,native,links,PORT/'tools/symlinkcheck.c')
    rows=[]
    with tempfile.TemporaryDirectory(prefix='kernel-io-') as directory:
        work=Path(directory);image=work/'disk.img';share=work/'share';share.mkdir()
        files=[dict(name='/init',data='FXN1:init:1\n') if native else dict(name='/init',host=str(build/'init.rxe'))]
        files += [dict(name='/bin/'+n,host=str(build/'apps'/(n+'.fxe') if native else build/(n+'.rxe'))) for n in ['sh','echo']]
        files += [dict(name='/bin/iocheck',host=str(probe)),dict(name='/dev/tty',mode=0o20666,device=0x200),dict(name='/dev/null',mode=0o20666,device=0x100)]
        files += [dict(name='/etc/passwd',data='root:x:0:0:root:/:/bin/sh\n')]
        if not a.benchmark_only:
            files += [dict(name='/bin/selectcheck',host=str(readiness)),dict(name='/bin/symlinkcheck',host=str(links)),
                      dict(name='/bin/rift',data='placeholder'),dict(name='/bin/vi',mode=0o120777,data='rift'),
                      dict(name='/bin/ln',mode=0o120777,data='echo')]
        files += [dict(name='/dev/root',mode=0o60600,device=0)]
        files += [dict(name='/dev/'+n,mode=0o20666,device=d) for n,d in [('ptyp0',0x300),('ttyp0',0x202)]]
        manifest=work/'files.json';manifest.write_text(json.dumps(files))
        run(['node','--input-type=module','-e',r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,image,build,native]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest,'utf8'));
for(const f of files)if(f.host){f.data=fs.readFileSync(f.host);delete f.host;}else if(typeof f.data==='string')f.data=Buffer.from(f.data);
const root=rootImage(files,16384,256);
atomicWrite(image,native==='1'?root:fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
''',manifest,image,build,int(native)])
        command=[exe,'--run',image,'--share',share] if native else [exe,build/'bios.bin','--disk',image,'--memory','16384000','--net','off','--jit','--timeout','600']
        for boot in range(2):
            g=Guest(command,a.os)
            try:
                g.raw(g.prompt)
                if not boot:
                    for repeat in range(a.repetitions):
                        flags=(' bench' if a.benchmark_only else ' full')+(' shared' if native else '')
                        text=g.shell('iocheck'+flags+'; echo IO_STATUS:$?',seconds=240).decode(errors='replace')
                        assert '\nPROBE_OK\n' in text and '\nIO_STATUS:0\n' in text,text
                        rows += [dict(name=n,count=int(c),microseconds=int(us),repetition=repeat) for n,c,us in re.findall(r'MEASURE (\w+) (\d+) (\d+)',text)]
                        print(a.os,'I/O repetition',repeat+1,'passed',flush=True)
                    if not a.benchmark_only:
                        text=g.shell('selectcheck; echo SELECT_STATUS:$?',seconds=180)
                        assert b'\nSELECT_OK\n' in text and b'\nSELECT_STATUS:0\n' in text,text
                        text=g.shell('symlinkcheck; echo LINK_STATUS:$?',seconds=180)
                        assert b'SYMLINK_OK' in text and b'\nLINK_STATUS:0\n' in text,text
                        text=g.shell('iocheck readonly; echo RO_STATUS:$?',seconds=120)
                        assert b'IO_READONLY_OK' in text and b'\nRO_STATUS:0\n' in text,text
                    g.shell('echo preserved > /saved',seconds=60)
                else:
                    # Existing echo uses a shell redirection; validate saved data
                    # through the same guest syscalls with no extra utility.
                    text=g.shell('iocheck persist',seconds=60)
                    assert b'\nIO_PERSIST_OK\n' in text,text
                    if not a.benchmark_only:
                        text=g.shell('symlinkcheck persist',seconds=60)
                        assert b'\nSYMLINK_PERSIST_OK\n' in text,text
                g.send(b'shutdown\r');g.process.wait(timeout=30);assert g.process.returncode==0
            finally:
                (out/f'guest-{boot}.log').write_bytes(g.log);g.close()
            check_disk(image,native)
    result=dict(executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),measurements=rows,
      medians={name:statistics.median(r['microseconds']/r['count'] for r in rows if r['name']==name) for name in sorted({r['name'] for r in rows})})
    result['kernel_sha256']=hashlib.sha256((exe if native else build/'FUZIX.SYS').read_bytes()).hexdigest()
    if not native:result['bios_sha256']=hashlib.sha256((build/'bios.bin').read_bytes()).hexdigest()
    result['system']=a.os
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['medians'],indent=2))
if __name__=='__main__':main()
