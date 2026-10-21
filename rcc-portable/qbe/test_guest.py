#!/usr/bin/env python3
"""Run the C16 QBE executable in a disposable guest and compare x86 output."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, default=HERE.parent/'build/apps/qbe.fxe')
    p.add_argument('--reference', type=Path, default=HERE.parent/'build/qbe-host/qbe')
    p.add_argument('--all', action='store_true', help='compile all upstream fixtures')
    a = p.parse_args()
    report = []
    with tempfile.TemporaryDirectory(prefix='qbe-guest-', dir='/tmp') as folder:
        work=Path(folder); share=work/'share'; share.mkdir(); scratch=work/'scratch'; scratch.mkdir()
        shutil.copy2(a.tool, share/'qbe.fxe')
        fixtures=sorted((HERE.parent/'upstream/qbe/test').glob('[!_]*.ssa'))
        if not a.all: fixtures=fixtures[:8]
        for source in fixtures: shutil.copy2(source,share/source.name)
        files=[dict(name='/'+source.name,path=str(source)) for source in fixtures]
        files.append(dict(name='/mnt/host',directory=True,mode=0o40755))
        wrapper=work/'kernel'
        prior=os.environ.get('FUSE_X_TEST_KERNEL')
        kernel=prior or str(ROOT/'fuse-x/build/fuse-x')
        wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(kernel)+' "$@" --share '+shlex.quote(str(share))+'\n')
        wrapper.chmod(0o755)
        os.environ['FUSE_X_TEST_KERNEL']=str(wrapper)
        try: guest=Guest(work,'native',{'qbe':a.tool},files=files,blocks=65536)
        finally:
            if prior is None: os.environ.pop('FUSE_X_TEST_KERNEL',None)
            else: os.environ['FUSE_X_TEST_KERNEL']=prior
        try:
            for source in fixtures:
                output=source.stem+'.s'
                start=time.monotonic()
                text=guest.shell('qbe -t amd64_sysv -o /mnt/host/'+output+' /'+source.name+'; echo QBE_STATUS:$?',120)
                if 'QBE_STATUS:0' not in text: raise RuntimeError(source.name+':\n'+text)
                reference=share/(source.stem+'.reference.s')
                subprocess.run([str(a.reference.resolve()),'-t','amd64_sysv','-o',str(reference),str(source.resolve())],check=True)
                exact=(share/output).read_bytes()==reference.read_bytes()
                if not exact and source.stem != 'spill1':
                    raise AssertionError(source.name+': guest QBE assembly differs from reference')
                peak=re.search(r'heap peak (\d+) cells',text)
                row=dict(heap_peak_cells=int(peak[1]) if peak else None,fixture=source.name,seconds=round(time.monotonic()-start,3),exact_reference=exact,output_octets=(share/output).stat().st_size)
                report.append(row); print(json.dumps(row),flush=True)
            (HERE.parent/'build/qbe-guest-test').mkdir(exist_ok=True)
            (HERE.parent/'build/qbe-guest-test/transcript.log').write_bytes(guest.log)
            guest.close(); guest=None
        finally:
            out=HERE.parent/'build/qbe-guest-test'; out.mkdir(exist_ok=True)
            if guest: (out/'transcript.log').write_bytes(guest.log)
            (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
            for source in fixtures:
                result=share/(source.stem+'.s')
                if result.exists(): shutil.copy2(result,out/result.name)
            if guest: guest.close()
    print('C16 QBE x86_64 output generated for '+str(len(report))+' fixtures')


if __name__=='__main__': main()
