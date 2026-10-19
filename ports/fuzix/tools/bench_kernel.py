#!/usr/bin/env python3
"""Measure the same kernel workload on disposable disks; no tool/kernel rebuild."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import tempfile
from bench_foundations import ROOT, PORT, build_probe, run, Guest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    parser.add_argument('--kernel',type=Path,help='alternate SYS1 kernel for a baseline comparison')
    parser.add_argument('--probe',type=Path,help='reuse the exact RXE probe from a previous run')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--engine',choices=('jit','interpreter'),default='jit')
    args=parser.parse_args()
    if args.repetitions<1:parser.error('--repetitions must be positive')
    build=args.build.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    kernel=(args.kernel or build/'FUZIX.SYS').resolve()
    source=PORT/'tools/kernelbench.c';exe=output/'kernelbench.rxe'
    if args.probe:
        if args.probe.resolve()!=exe:
            shutil.copyfile(args.probe.resolve(),exe)
    else:
        build_probe(build,False,exe,source)
    measurements=[]
    with tempfile.TemporaryDirectory(prefix='kernel-bench-') as temporary:
        work=Path(temporary);image=work/'disk.img'
        files=[dict(name='/init',host=str(build/'init.rxe'))]
        files += [dict(name='/bin/'+n,host=str(build/(n+'.rxe'))) for n in ['sh','echo']]
        files += [dict(name='/bin/kernelbench',host=str(exe)),dict(name='/dev/tty',mode=0o20666,device=0x200)]
        manifest=work/'manifest.json';manifest.write_text(json.dumps(files))
        run(['node','--input-type=module','-e',r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,image,kernel]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest,'utf8'));
for(const f of files)if(f.host){f.data=fs.readFileSync(f.host);delete f.host;}
const root=rootImage(files,16384,256);
atomicWrite(image,fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(kernel)},{name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
''',manifest,image,kernel])
        command=[ROOT/'target/release/rvm',build/'bios.bin','--disk',image,'--memory','16384000',
                 '--net','off','--timeout','600','--report',output/'report.json']
        if args.engine=='jit':command+=['--jit']
        guest=Guest(command,'fuzix')
        try:
            guest.raw(guest.prompt)
            for repetition in range(args.repetitions):
                text=guest.shell('kernelbench; echo BENCH_STATUS:$?',seconds=180).decode(errors='replace')
                assert 'PROBE_OK' in text and 'BENCH_STATUS:0' in text,text
                found=re.findall(r'MEASURE (\w+) (\d+) (\d+)',text)
                assert len(found)==8,text
                for name,count,micros in found:
                    measurements.append(dict(name=name,count=int(count),microseconds=int(micros),repetition=repetition))
                print('Kernel benchmark repetition',repetition+1,'passed',flush=True)
            guest.send(b'shutdown\r');guest.process.wait(timeout=30)
            assert guest.process.returncode==0
        finally:
            (output/'guest.log').write_bytes(guest.log);guest.close()
    report=json.loads((output/'report.json').read_text())
    result=dict(engine=args.engine,kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                probe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                instructions=report['instructions'],jit=report.get('jit'),measurements=measurements,
                median_microseconds_per_operation={name:statistics.median(
                    r['microseconds']/r['count'] for r in measurements if r['name']==name)
                    for name in sorted({r['name'] for r in measurements})},
                scope='Instruction total includes boot, shell and executable loading. Wall times include JIT and host scheduling; not a latency guarantee.')
    (output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ['instructions','median_microseconds_per_operation']},indent=2))
if __name__=='__main__':main()
