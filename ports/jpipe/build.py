#!/usr/bin/env python3
"""Build one multicall QuickJS pipeline tool; never install normal disk images."""
import argparse,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'ports/quickjs'))
from explore import CORE,PORT,guest_build,run,sha
from vendor import verify
ALIASES=('jlines','jobj','jmap','jfilter','jreduce','jsort','jtext')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('host','native','ripple','both'),default='host')
p.add_argument('--build',type=Path,default=ROOT/'build/jpipe')
p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
p.add_argument('--cc',default=os.environ.get('CC','/usr/bin/clang' if sys.platform=='darwin' else 'cc'))
a=p.parse_args();verify()
sources=[HERE/(name+'.c') for name in ('main','records','engine')]
for target in (('native','ripple') if a.target=='both' else (a.target,)):
    out=a.build.resolve()/target;out.mkdir(parents=True,exist_ok=True)
    (out/'build.json').unlink(missing_ok=True)
    if target=='host':
        binary=out/'jpipe'
        flags=['-std=gnu11','-O2','-fwrapv','-D_GNU_SOURCE','-DCONFIG_VERSION="2026-06-04"','-I'+str(PORT/'upstream')]
        if sys.platform=='darwin':flags+=['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
        run([*shlex.split(a.cc),*flags,*sources,*[PORT/'upstream'/(name+'.c') for name in CORE],'-lm','-o',binary],out/'build.log',timeout=180)
    else:
        compiler=out/'rcc-pinned'
        if not compiler.exists() or sha(compiler)!=sha(a.compiler):shutil.copy2(a.compiler,compiler)
        binary=guest_build(out,compiler,target,'runtime',application_sources=sources,output_name='jpipe')
    for name in ALIASES:
        link=out/name
        if link.is_symlink():link.unlink()
        if link.exists():raise RuntimeError('refusing to overwrite non-alias '+str(link))
        link.symlink_to(binary.name)
    (out/'build.json').write_text(json.dumps(dict(target=target,binary=str(binary),sha256=sha(binary),aliases=ALIASES),indent=2)+'\n')
    print('Built pipeline commands:',binary,flush=True)
