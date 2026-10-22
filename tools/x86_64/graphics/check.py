#!/usr/bin/env python3
"""Focused native GX and screenshot-client checks; no VM or device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/x86_64-graphics')
    p.add_argument('--cc',default=shutil.which('clang') or 'cc')
    a=p.parse_args();build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
    flags=['-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all']
    # Pinned installed SDK avoids the newer .tbd format unsupported by this ld.
    sdk=Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
    if sys.platform=='darwin' and sdk.exists():flags+=['-isysroot',str(sdk)]
    checks={}
    for name in ('gx-backend','screenshot-client'):
        executable=build/(name+'-check')
        subprocess.run([a.cc,*flags,str(ROOT/'tools/x86_64/tests'/(name+'.c')),'-o',str(executable)],check=True)
        command=[str(executable)]
        if name=='screenshot-client':command.append(str(build/'screenshot-test.ppm'))
        result=subprocess.run(command,capture_output=True,text=True,check=True)
        checks[name]={'status':'passed','stdout':result.stdout,'stderr':result.stderr}
        print(result.stdout,end='')
    subprocess.run([a.cc,'--target=x86_64-unknown-none-elf','-ffreestanding','-fno-builtin','-mno-red-zone','-std=c11','-O2','-Wall','-Wextra','-Werror','-c',str(ROOT/'tools/x86_64/boot/gx.c'),'-o',str(build/'gx-check.o')],check=True)
    inputs=['boot/gx.c','boot/gx.h','graphics/screenshot.c','tests/gx-backend.c','tests/screenshot-client.c']
    report={'status':'passed','sanitizer':'undefined, fatal','checks':checks,'freestanding_compile':'x86_64-unknown-none-elf','sources':{name:hashlib.sha256((ROOT/'tools/x86_64'/name).read_bytes()).hexdigest() for name in inputs}}
    (build/'host-result.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
