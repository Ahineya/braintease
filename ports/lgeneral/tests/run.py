#!/usr/bin/env python3
"""Exercise the real LGeneral engine in a disposable, offline Fuse-X kernel."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def engine(binary,data,output,all_scenarios=False):
    output.mkdir(parents=True,exist_ok=True)
    files=[dict(name='/dev/gfx',mode=0o20600,device=0x400)]
    files += [dict(name='/usr/share/lgeneral/'+str(p.relative_to(data)),path=str(p.resolve()),mode=0o100644)
              for p in sorted(data.rglob('*')) if p.is_file()]
    with tempfile.TemporaryDirectory(prefix='lgeneral-engine-') as temp:
        saved=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(Path(temp)/'frames'),RCC_NATIVE_AUDIO='off')
        try:g=Guest(temp,'native',{'lgeneral-test':binary},files,blocks=65536,timeout=1800)
        finally:os.environ.clear();os.environ.update(saved)
        started=time.monotonic()
        try:
            command='HOME=/tmp LGENERAL_TRACE=1 '
            if all_scenarios:command+='LGENERAL_TEST_ALL_SCENARIOS=1 '
            result=g.shell(command+'/bin/lgeneral-test; echo LGENERAL_EXIT:$?',seconds=1500)
            (output/'guest.log').write_bytes(g.log)
            assert 'LGENERAL ENGINE PASS' in result and 'LGENERAL_EXIT:0' in result,result[-12000:]
            assert '\nFAIL lgeneral:' not in result,result
            assert 'panic:' not in result,result
        finally:
            (output/'guest.log').write_bytes(g.log)
            g.close()
    info=dict(status='passed',binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              checks=[line.strip() for line in result.splitlines() if line.startswith(('PASS lgeneral:', 'LGENERAL AI', 'LGENERAL LOAD', 'LGENERAL SCENARIO', 'UNIT_TIMING'))],
              seconds=round(time.monotonic()-started,3),all_scenarios=all_scenarios)
    (output/'result.json').write_text(json.dumps(info,indent=2)+'\n');return info


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,default=ROOT/'build/lgeneral-engine/lgeneral-test.fxe')
    p.add_argument('--data',type=Path,default=ROOT/'build/lgeneral-engine/data')
    p.add_argument('--output',type=Path,default=ROOT/'build/lgeneral-engine/engine-test')
    p.add_argument('--all-scenarios',action='store_true');a=p.parse_args()
    print(json.dumps(engine(a.binary,a.data,a.output,a.all_scenarios),indent=2))


if __name__=='__main__':main()
