#!/usr/bin/env python3
"""Build/check allocator scaling probes on private native/RVM disks."""
import argparse
import hashlib
import json
import platform
import shutil
from pathlib import Path
import re
import statistics
import sys
import tempfile
from test_symlinks import run,check_disk
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
sys.path.insert(0,str(ROOT/'rfl/tools'))
from test_os import Guest


def build_probe(build,native,output,source):
    if native:
        run(['python3',ROOT/'fuse-x/cc.py',source,'--build',build,'-o',output])
    else:
        assembly=output.with_suffix('.asm');obj=output.with_suffix('.pobj')
        run([ROOT/'target/release/rcc','compile',source,'-o',assembly,'--bank-size','64000',
             '-DRIPPLE_MMU=1','-DNO_64BIT',*['-I'+str(p) for p in
             (PORT/'ripple/include',PORT/'upstream/Library/include',ROOT/'runtime/include',PORT/'ripple')]])
        run([ROOT/'src/ripple-asm/target/release/rasm','assemble',assembly,'-o',obj,'--bank-size','64000','--max-immediate','65535'])
        sdk=json.loads(run(['make','-s','--no-print-directory','-C',PORT,'BUILD='+str(build),'rfl-sdk-info']))
        run([ROOT/'src/ripple-asm/target/release/rlink',sdk['entry'],obj,*sdk['libraries'],'-f','rxe','--bank-size','64000','-o',output])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--engine',choices=('interpreter','jit'),default='jit')
    p.add_argument('--os',choices=('fuzix','fuse-x','both'),default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--source',type=Path,default=PORT/'tools/heapcheck.c')
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--measure-only',action='store_true')
    p.add_argument('--repetitions',type=int,default=3)
    args=p.parse_args();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    assert args.repetitions>0 and not(args.build_only and args.measure_only)
    results={}
    for system in (['fuse-x','fuzix'] if args.os=='both' else [args.os]):
        native=system=='fuse-x';build=(args.native_build if native else args.build).resolve()
        probe=args.output/('heapcheck.'+('fxe' if native else 'rxe'))
        if not args.measure_only:
            build_probe(build,native,probe,args.source.resolve())
            shutil.copyfile(args.source, args.output / (system + '-source.c'))
        if args.build_only:continue
        with tempfile.TemporaryDirectory(prefix='foundation-bench-') as t:
            work=Path(t);image=work/'disk.img'
            files=[dict(name='/init',data='FXN1:init:1\n') if native else dict(name='/init',host=str(build/'init.rxe'))]
            files += [dict(name='/bin/'+n,host=str(build/'apps'/(n+'.fxe') if native else build/(n+'.rxe'))) for n in ['sh','echo']]
            files += [dict(name='/bin/heapcheck',host=str(probe)),dict(name='/dev/tty',mode=0o20666,device=0x200)]
            manifest=work/'manifest.json';manifest.write_text(json.dumps(files))
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
            command=[build/'fuse-x','--run',image] if native else [ROOT/'target/release/rvm',build/'bios.bin','--disk',image,'--memory','16384000','--net','off','--timeout','900','--report',str(args.output/'rvm-report.json'),*(['--jit'] if args.engine=='jit' else [])]
            guest=Guest(command,system);measurements=[]
            try:
                guest.raw(guest.prompt)
                for repetition in range(args.repetitions):
                    out=guest.shell('heapcheck; echo HEAP_STATUS:$?',seconds=240).decode(errors='replace')
                    assert '\nHEAP_OK\n' in out and '\nHEAP_STATUS:0\n' in out,out
                    for count,micros in re.findall(r'^HEAP_BATCH (\d+) (\d+)$',out,re.M):
                        measurements.append(dict(count=int(count),microseconds=int(micros),repetition=repetition))
                    assert len(measurements)==3*(repetition+1),out
                    print(system, 'repetition', repetition + 1, measurements[-3:], flush=True)
                guest.send(b'shutdown\r');guest.process.wait(timeout=30);assert guest.process.returncode==0
            finally:
                (args.output/(system+'.log')).write_bytes(guest.log);guest.close()
            check_disk(image,native)
            results[system]=dict(measurements=measurements,medians={n:statistics.median(v['microseconds'] for v in measurements if v['count']==n) for n in [128,512,1024]},probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest())
            print(system,results[system]['medians'],flush=True)
    if results:
        results['platform']=platform.platform()
        results['rvm_engine']=args.engine
        results['source_sha256']=hashlib.sha256(args.source.read_bytes()).hexdigest()
        results['compiler_sha256']=hashlib.sha256((ROOT/'target/release/rcc').read_bytes()).hexdigest()
        (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')


if __name__=='__main__':main()
