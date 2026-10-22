#!/usr/bin/env python3
"""Exercise shared Graphite window-focus suspension without a guest image."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from adapt import sources

ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,default=ROOT/'build/fusede-run/lvgl')
parser.add_argument('--output',type=Path,default=ROOT/'build/fusede-focus/native')
args=parser.parse_args()
build=args.build.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
config=output/'config';config.mkdir(exist_ok=True)
(config/'lv_conf.h').write_bytes((build/'config/lv_conf.h').read_bytes())
flags=['-std=c99','-O1','-g','-Wall','-Wextra','-Wno-deprecated-declarations',
       '-fsanitize=undefined','-fno-sanitize-recover=all','-DLV_CONF_INCLUDE_SIMPLE=1',
       '-I'+str(build/'src'),'-I'+str(config),'-I'+str(PORT),'-I'+str(ROOT/'ports/graphics/include')]
compiler=os.environ.get('CC','cc')
if sys.platform=='darwin':
    sdk=os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
    sdk_compiler=Path(sdk).parent.parent/'usr/bin/clang'
    compiler=os.environ.get('CC') or (str(sdk_compiler) if sdk_compiler.is_file() else
                                    subprocess.check_output(['xcrun','--find','clang'],text=True).strip())
    flags+=['-isysroot',sdk]
base=hashlib.sha256(repr(flags).encode()+(config/'lv_conf.h').read_bytes())
for path in sorted((build/'src').rglob('*.h')):base.update(path.read_bytes())
prefix=base.digest()
product_headers=b''.join(path.read_bytes() for path in sorted(PORT.glob('*.h')))
product_headers+=b''.join(path.read_bytes() for path in sorted((ROOT/'libraries/text').glob('*')) if path.is_file())
def compile(path):
    extra=product_headers if not path.is_relative_to(build/'src') else b''
    digest=hashlib.sha256(prefix+path.read_bytes()+extra).hexdigest()[:16]
    target=output/(path.stem+'-'+digest+'.o')
    if not target.exists():
        result=subprocess.run([compiler,*flags,'-c',str(path),'-o',str(target)],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
    return target
inputs=[*sources(build/'src'),*[PORT/'fonts'/f'font-{n}.c' for n in json.loads((PORT/'fonts/generated.json').read_text())['sizes']],
        PORT/'fonts/symbols.c',PORT/'icons/data.c',PORT/'ui-scale.c',PORT/'ui.c',PORT/'ui-data.c',
        PORT/'ui-editor.c',ROOT/'libraries/text/text.c',ROOT/'libraries/unicode/unicode.c',
        PORT/'host-gx.c',PORT/'focus-test.c']
with ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile,inputs))
program=output/'focus-test'
subprocess.run([compiler,*flags,*map(str,objects),'-lm','-o',str(program)],check=True)
results=[]
for scale in (1,2):
    run=subprocess.run([str(program)],env=dict(os.environ,LVGL_SCALE=str(scale)),capture_output=True,text=True,check=True,timeout=60)
    print(f'{scale}x: '+run.stdout.strip());results.append(dict(scale=scale,output=run.stdout))
report=dict(status='passed',sanitizer='undefined',results=results,
            input_sha256={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs})
(output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
