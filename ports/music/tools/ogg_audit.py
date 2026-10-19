#!/usr/bin/env python3
"""Compile-audit a local Tremor nobyte checkout and benchmark its MDCT in guests."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
UNITS='mdct dsp info floor1 floor0 vorbisfile res012 mapping0 codebook framing bitwise floor_lookup'.split()
TYPES='''#include <stdint.h>
typedef int64_t ogg_int64_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
'''


def run(command):
    return subprocess.run(list(map(str,command)),cwd=ROOT,capture_output=True,text=True)


def checked(command):
    result=run(command)
    if result.returncode:raise RuntimeError(result.stderr[-3000:]+result.stdout[-2000:])
    return result.stdout


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True,help='public Xiph Tremor nobyte checkout; not vendored into production')
    parser.add_argument('--benchmark',action='store_true')
    parser.add_argument('--rounds',type=int,default=32)
    args=parser.parse_args()
    if not 1<=args.rounds<=500:parser.error('--rounds must be 1..500')
    source=args.source.resolve(strict=True)
    output=ROOT/'build/music-tests/ogg-audit';output.mkdir(parents=True,exist_ok=True)
    adapted=output/'source'
    if adapted.exists():shutil.rmtree(adapted)
    shutil.copytree(source,adapted,ignore=shutil.ignore_patterns('.git'))
    (adapted/'config_types.h').write_text(TYPES)
    common=['-DSTIN=static','-DBYTE_ORDER=1234','-DLITTLE_ENDIAN=1234','-DBIG_ENDIAN=4321']
    includes=[]
    for path in [adapted,ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include']:
        includes+=['-I',str(path)]
    compiler=ROOT/'target/release/rcc'
    flags=['--bank-size','64000','-DRIPPLE_MMU=1',*common,*includes]
    results=[]
    for name in UNITS:
        result=run([compiler,'compile',adapted/(name+'.c'),'-o',output/(name+'.asm'),*flags])
        text=result.stdout+result.stderr
        (output/(name+'.log')).write_text(text)
        diagnostics=[line for line in text.splitlines() if re.search(r'\bwarning:|\berror(?:\s|:)|\bimplicit\b',line,re.I)]
        results.append(dict(unit=name,compiled=result.returncode==0,diagnostics=diagnostics[:8]))
        print(name+': '+('compiled' if not result.returncode else 'needs adaptation'),flush=True)
    report=dict(source_commit=checked(['git','-C',source,'rev-parse','HEAD']).strip(),
                adaptations=['explicit 16/32/64-bit types','inline fallback','explicit little endian'],units=results)
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if args.benchmark and results[0]['compiled']:
        probe=ROOT/'ports/music/tests/ogg_mdct_probe.c'
        native=output/'oggprobe.fxe'
        checked([sys.executable,ROOT/'fuse-x/cc.py',probe,adapted/'mdct.c',*common,'-I',adapted,'-o',native])
        build=ROOT/'build/fuzix'
        sdk=json.loads(checked(['make','-s','--no-print-directory','-C',ROOT/'ports/fuzix',f'BUILD={build}','rfl-sdk-info']))
        assembler=ROOT/'src/ripple-asm/target/release/rasm'
        checked([compiler,'compile',probe,'-o',output/'probe.asm',*flags])
        for name in ['mdct','probe']:
            checked([assembler,'assemble',output/(name+'.asm'),'-o',output/(name+'.pobj'),'--bank-size','64000','--max-immediate','65535'])
        ripple=output/'oggprobe.rxe'
        checked([ROOT/'src/ripple-asm/target/release/rlink',sdk['entry'],output/'mdct.pobj',output/'probe.pobj',*sdk['libraries'],'-f','rxe','--bank-size','64000','-o',ripple])
        hostcc='cc';hostflags=[]
        if sys.platform=='darwin':
            hostcc=checked(['xcrun','--find','clang']).strip()
            hostflags=['-isysroot',checked(['xcrun','--sdk','macosx','--show-sdk-path']).strip()]
        reference=output/'oggprobe.host'
        checked([hostcc,*hostflags,'-O2','-fwrapv','-DSTIN=static',*common[1:],'-I',adapted,probe,adapted/'mdct.c','-o',reference])
        host=checked([reference,args.rounds]).strip().split()
        expected=int(host[3])
        measurements={'host_us':int(host[2]),'checksum':expected}
        sys.path.insert(0,str(ROOT/'ports/tls/tests'))
        from guest import Guest
        for target,artifact in [('native',native),('ripple',ripple)]:
            with tempfile.TemporaryDirectory(prefix='ogg-mdct-') as directory:
                guest=Guest(directory,target,{'oggprobe':artifact})
                try:text=guest.shell('oggprobe '+str(args.rounds),seconds=180)
                finally:guest.close()
                match=re.search(r'OGG_MDCT (\d+) (\d+) (\d+)',text)
                if not match:raise RuntimeError(text)
                elapsed,checksum=int(match[2]),int(match[3])
                if checksum!=expected:raise RuntimeError(f'{target} MDCT differs from the independent host C reference: {checksum} != {expected}')
                measurements[target+'_us']=elapsed
                # Two 2048-point transforms produce 1024 stereo frames.
                ratio=args.rounds*1024/2/44100/(max(1,elapsed)/1000000)
                print(f'{target}: MDCT-only stereo budget {ratio:.2f}x real time; checksum matches host C',flush=True)
        report['mdct']=measurements
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(output/'report.json')


if __name__=='__main__':main()
