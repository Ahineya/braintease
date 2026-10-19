#!/usr/bin/env python3
"""Accelerated libc and pure-software comparison inside both real kernels."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from common import ROOT, compiler_snapshot, software_reference
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('--target',choices=['native','ripple','both'],default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/float-accel')
    args=p.parse_args();build=args.build.resolve();build.mkdir(parents=True,exist_ok=True)
    compiler=compiler_snapshot(args.compiler, build/'guest-rcc-pinned')
    report={'timing':'minimum of four warm batches, 500 operations each', 'compiler_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'targets':{}}
    reference=software_reference(build)
    inputs=[Path(__file__).with_name('guest.c'),reference]
    includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include']
    for target in (['native','ripple'] if args.target=='both' else [args.target]):
        binary=build/('mathcheck.'+('fxe' if target=='native' else 'rxe'))
        with (build/(target+'-test-build.log')).open('w') as log:
            def run(cmd):return subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True,env=dict(os.environ,RUST_LOG='error'))
            if target=='native':
                run([sys.executable,ROOT/'fuse-x/cc.py',*inputs,'--compiler',compiler,'--float-format','--build',build/'sdk-native','-o',binary])
            else:
                sdk=ROOT/'build/fuzix'
                make=['make','-s','--no-print-directory','-C',ROOT/'ports/fuzix','BUILD='+str(sdk),'RCC='+str(compiler)]
                run([*make,'-j4','rfl-sdk','float-sdk'])
                info=json.loads(subprocess.check_output(list(map(str,[*make,'rfl-sdk-info']))))
                floats=json.loads(subprocess.check_output(list(map(str,[*make,'float-sdk-info']))))
                objects=[]
                for i,source in enumerate(inputs):
                    asm=build/f'input{i}.asm';obj=asm.with_suffix('.pobj');objects.append(obj)
                    run([compiler,'compile',source,'-DRIPPLE_MMU=1',*[v for d in includes for v in ['-I',d]],'-o',asm])
                    run([ROOT/'src/ripple-asm/target/release/rasm','assemble',asm,'-o',obj,'--bank-size','64000','--max-immediate','65535'])
                libs=[v for v in info['libraries'] if not v.endswith('/libc/format.pobj')]+floats['libraries']
                run([ROOT/'src/ripple-asm/target/release/rlink',info['entry'],*objects,*libs,'-f','rxe','--bank-size','64000','-o',binary])
        with tempfile.TemporaryDirectory(prefix='float-accel-') as directory:
            guest=Guest(directory,target,{'mathcheck':binary})
            try:
                output=guest.shell('mathcheck; echo MATH_STATUS:$?',240)
                if 'failures=0' not in output or 'MATH_STATUS:0' not in output or 'FAIL ' in output:raise RuntimeError(output)
                timings=[dict(operation=op,accelerated_ms=int(a),software_ms=int(b)) for op,a,b in re.findall(r'MATH_BENCH (\w+) (\d+) (\d+)',output)]
                if len(timings)!=6:raise RuntimeError('Missing benchmark rows: '+output)
                print(target,output,flush=True)
            finally:
                guest.close();(build/(target+'-guest.log')).write_bytes(guest.log)
        report['targets'][target]={'timings':timings,'filesystem_check':'passed','output':output}
        (build/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
