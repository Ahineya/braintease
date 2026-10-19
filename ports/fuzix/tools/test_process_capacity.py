#!/usr/bin/env python3
"""Fill/reuse all 64 process slots on disposable native or RVM root images."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',action='store_true')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--interpreter',action='store_true')
    a=p.parse_args();build=(a.native_build if a.native else a.build).resolve()
    with tempfile.TemporaryDirectory(prefix='process-capacity-') as temporary:
        work=Path(temporary);image=work/'disk.img';stage=work/'stage';stage.mkdir()
        names=['sh','ps','top','free','uptime','echo','sleep','finecheck','proccheck']
        if a.native:
            (stage/'native-hello.fxe').symlink_to(build/'apps/native-hello.fxe')
            for name in ['proccheck','finecheck']:
                subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'tools'/(name+'.c')),
                                '--build',str(build),'-o',str(stage/(name+'.fxe'))],check=True)
            for name in names[:-2]:(stage/(name+'.fxe')).symlink_to(build/'apps'/(name+'.fxe'))
            (work/'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
            subprocess.run(['node',str(ROOT/'fuse-x/image.mjs'),'create',str(image),'--apps',str(stage)],check=True)
            command=[str(build/'fuse-x'),'--run',str(image),'--one-shot']
        else:
            names+=['init']
            for name in names:(stage/(name+'.rxe')).symlink_to(build/(name+'.rxe'))
            for name in ['FUZIX.SYS','bios.bin']:(stage/name).symlink_to(build/name)
            (stage/'apps.json').write_text(json.dumps(names))
            subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(stage),str(image)],check=True)
            command=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
                     '--memory','16384000','--headless','--net','off','--timeout','180']
            if not a.interpreter:command+=['--jit']
        commands='proccheck\nfinecheck\nps -A\ntop -b -n 1\nfree\nuptime\n'
        commands+='for n in '+' '.join(map(str,range(40)))+'; do sleep 10 & done\njobs\nwait\necho JOB_CAPACITY_OK\nshutdown\n'
        env=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='180')
        # The Riplix process table is authoritative, regardless of a caller's
        # standalone-runtime limit used for unrelated native compiler fixtures.
        env.pop('RCC_NATIVE_MAX_PROCESSES',None)
        if not a.native:command+=['--input',commands]
        result=subprocess.run(command,input=commands if a.native else None,text=True,
                              stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,timeout=190)
        log=build/('process-capacity-native.log' if a.native else 'process-capacity-rvm.log')
        log.write_text(result.stdout)
        assert result.returncode==0,result.stdout[-8000:]
        assert '(64 processes max)' in result.stdout,result.stdout[-8000:]
        assert result.stdout.count('PROCCHECK_FULL 64 processes; last slot 63 PID ')==2,result.stdout[-8000:]
        assert 'PROCCHECK_OK 64 slots, exhaustion, high-slot select and reuse' in result.stdout,result.stdout[-8000:]
        assert 'errors and 64 slots passed' in result.stdout and 'FINE_OK ' in result.stdout,result.stdout[-8000:]
        assert 'FINE_FAILED' not in result.stdout,result.stdout[-8000:]
        assert 'JOB_CAPACITY_OK' in result.stdout and '[40]' in result.stdout,result.stdout[-8000:]
        assert 'job table full' not in result.stdout and 'too many jobs' not in result.stdout,result.stdout[-8000:]
        assert 'PROCCHECK_FAILED' not in result.stdout and 'panic:' not in result.stdout,result.stdout[-8000:]
        assert ('filesystem synced; session stopped' if a.native else 'disk synced; power off.') in result.stdout,result.stdout[-8000:]
        sys.path.insert(0,str(ROOT/'fuse-x'))
        if a.native:
            from test_support import check_root
            check_root(image)
        else:
            subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
        print(('Fuse-X' if a.native else 'RVM/Riplix')+': 64 occupied processes, reports, high-slot timers, COW and two rounds of recovery passed')

if __name__=='__main__':main()
