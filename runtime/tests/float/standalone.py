#!/usr/bin/env python3
"""Compare direct-mailbox arithmetic with integer helpers, on the real RVM JIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from common import ROOT, compiler_snapshot, software_reference
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
args=parser.parse_args()
build=ROOT/'build/float-accel';build.mkdir(parents=True,exist_ok=True)
compiler=compiler_snapshot(args.compiler,build/'standalone-rcc-pinned')
reference=software_reference(build)
with (build/'standalone-build.log').open('w') as log:
 def run(cmd):return subprocess.run(list(map(str,cmd)),check=True,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,RUST_LOG='error'))
 run(['make','-s','-C',ROOT/'runtime','-j4','RCC='+str(compiler)])
 objects=[]
 for name,source in [('standalone',Path(__file__).with_name('guest.c')),('reference',reference)]:
  asm=build/(name+'.asm');obj=asm.with_suffix('.pobj');objects.append(obj)
  run([compiler,'compile',source,'-DFLOAT_STANDALONE=1','-I',ROOT/'runtime/include','-o',asm])
  run([ROOT/'src/ripple-asm/target/release/rasm','assemble',asm,'-o',obj,'--bank-size','64000','--max-immediate','65535'])
 run([ROOT/'src/ripple-asm/target/release/rlink',ROOT/'runtime/crt0.pobj',ROOT/'runtime/libruntime.par',*objects,'-f','binary','--bank-size','64000','-o',build/'standalone.bin'])
temporary=tempfile.TemporaryDirectory(prefix='float-standalone-')
output=subprocess.check_output([str(ROOT/'target/release/rvm'),str(build/'standalone.bin'),'--jit','--disk',str(Path(temporary.name)/'disk.img'),'--net','off','--timeout','180'],text=True,stderr=subprocess.STDOUT,timeout=200)
(build/'standalone.log').write_text(output)
if 'failures=0' not in output or 'FAIL ' in output:raise RuntimeError(output)
rows=[dict(operation=n,accelerated_ms=int(a),software_ms=int(b)) for n,a,b in re.findall(r'MATH_BENCH (\w+) (\d+) (\d+)',output)]
assert len(rows)==6,output
(build/'standalone-report.json').write_text(json.dumps({'compiler_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'timing':'minimum of four warm batches, 5000 operations each','rows':rows,'output':output},indent=2)+'\n')
print(output)
