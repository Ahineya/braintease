#!/usr/bin/env python3
"""Pixel-reference, malformed-input and optional real C16 guest image checks."""
import argparse,json,os,shlex,subprocess,sys,tempfile
from pathlib import Path
from image_fixtures import ROOT,header
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def run(command,**kwargs):return subprocess.run(list(map(str,command)),check=True,**kwargs)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['host','native','ripple','both'],default='host');p.add_argument('--cc',default='cc')
 p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc');p.add_argument('--build',type=Path,default=ROOT/'build/image-test');p.add_argument('--skip-build',action='store_true')
 args=p.parse_args();build=args.build.resolve();build.mkdir(parents=True,exist_ok=True);compiler=args.compiler.resolve()
 run([sys.executable,ROOT/'libraries/image/adapt.py']);header(build/'fixtures')
 flags=['-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(build/'fixtures')]
 if sys.platform=='darwin':flags+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
 run([*shlex.split(args.cc),*flags,ROOT/'libraries/tests/image.c',ROOT/'libraries/image/decode.c','-o',build/'imagecheck'])
 run([build/'imagecheck'],timeout=90)
 run([*shlex.split(args.cc),*flags,'-O1',ROOT/'libraries/tests/image_malformed.c','-o',build/'malformed'])
 run([build/'malformed'],timeout=120)
 for target in (('native','ripple') if args.target=='both' else (() if args.target=='host' else (args.target,))):
  artifact=build/('imagecheck.fxe' if target=='native' else 'imagecheck.rxe');sdk=ROOT/'build/netsurf'/('sdk-worker-'+target)
  if not args.skip_build:
   with (build/(target+'-build.log')).open('w') as log:
    if target=='native':run([sys.executable,ROOT/'fuse-x/cc.py',ROOT/'libraries/tests/image.c',ROOT/'libraries/image/decode.c','-I'+str(build/'fixtures'),'--compiler',compiler,'--build',sdk,'-o',artifact],stdout=log,stderr=log)
    else:
     sdk_command=['make','-s','--no-print-directory','-C',str(ROOT/'ports/fuzix'),'BUILD='+str(sdk),'RCC='+str(compiler)]
     run([*sdk_command,'rfl-sdk','-j3'],stdout=log,stderr=log)
     info=json.loads(subprocess.check_output([*sdk_command,'rfl-sdk-info'],text=True));objects=[]
     includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',build/'fixtures'];asm=ROOT/'src/ripple-asm/target/release/rasm'
     for name,source in [('check',ROOT/'libraries/tests/image.c'),('decode',ROOT/'libraries/image/decode.c')]:
      ir=build/(name+'.ir');assembly=ir.with_suffix('.asm');obj=ir.with_suffix('.pobj')
      with ir.open('w') as out:run([compiler,'compile',source,'--target','ripple','-DRIPPLE_MMU=1','-DNO_64BIT',*[v for path in includes for v in ['-I',str(path)]],'--emit-ir'],stdout=out,stderr=log)
      run([compiler,'generate-asm','-i',ir,'--banked-globals','-o',assembly],stdout=log,stderr=log)
      run([asm,'assemble',assembly,'-o',obj,'--bank-size','64000','--max-immediate','65535'],stdout=log,stderr=log);objects.append(obj)
     run([asm.with_name('rlink'),info['entry'],*objects,*info['libraries'],'-f','rxe','--bank-size','64000','-o',artifact],stdout=log,stderr=log)
  with tempfile.TemporaryDirectory(prefix='image-guest-') as temp:
   g=Guest(temp,target,{'imagecheck':artifact},blocks=65536)
   try:
    text=g.shell('imagecheck; echo IMAGE_STATUS:$?',300);print(text,flush=True)
    assert 'failures=0' in text and 'IMAGE_STATUS:0' in text,text
   finally:(build/(target+'-guest.log')).write_bytes(g.log);g.close()
if __name__=='__main__':main()
