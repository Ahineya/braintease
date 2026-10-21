#!/usr/bin/env python3
"""Cross-build the same portable engine; Python is only a host build driver."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--target',choices=('fuse-x','x64'),default='fuse-x')
    ap.add_argument('--build',type=Path,default=ROOT/'build/pkg')
    args=ap.parse_args(); build=args.build.resolve(); build.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(ROOT/'ports/tls'))
    spec=importlib.util.spec_from_file_location('package_tls_build',ROOT/'ports/tls/build.py')
    tls=importlib.util.module_from_spec(spec); spec.loader.exec_module(tls)
    objects=tls.build(ROOT/'build/tls',ROOT/'target/release/rcc')
    target='fuse-x-arm64-macos-c16' if args.target=='fuse-x' else 'riplix-x64-c16'
    output=build/('pkg-engine.fxe' if args.target=='fuse-x' else 'pkg-engine-x64.rxe')
    command=[sys.executable,str(ROOT/'fuse-x/cc.py'),*[str(ROOT/'ports/pkg'/f'{name}.c') for name in ('main','sha256','format','store','repo','network','verify')],
             '--static','--build',str(build/('fuse-sdk' if args.target=='fuse-x' else 'x64-sdk')),
             '-DPKG_WOLFSSL','-DWOLFSSL_USER_SETTINGS',f'-DPKG_TARGET="{target}"',
             '-I',str(ROOT/'ports/tls'),'-I',str(ROOT/'ports/tls/config'),'-I',str(ROOT/'build/tls/source'),'-o',str(output)]
    for obj in objects: command+=['--object-ir',str(obj)]
    if args.target=='x64':command+=['--heap-profile','bounded','--linker',str(ROOT/'tools/x86_64/app_link.py')]
    subprocess.run(command,check=True,cwd=ROOT)

if __name__=='__main__':main()
