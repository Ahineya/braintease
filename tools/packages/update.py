#!/usr/bin/env python3
"""Build selected applications, package the catalog, optionally publish updates.

No kernel build or OS disk installation is performed. With --no-build this
packages existing artifacts (including all already-built applications).
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(Path(__file__).resolve().parent))
from catalog import Catalog, ALIASES, GUI

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('apps',nargs='*')
    ap.add_argument('--target',choices=('fuse-x','x64'),default='fuse-x')
    ap.add_argument('--build',type=Path)
    ap.add_argument('--no-build',action='store_true')
    ap.add_argument('--publish',action='store_true')
    args=ap.parse_args()
    if not args.apps and not args.no_build:ap.error('name applications, or use --no-build to package existing artifacts')
    build=(args.build or ROOT/('fuse-x/build' if args.target=='fuse-x' else 'build/pkg/x64-apps')).resolve()
    names=sorted({ALIASES.get(n,n) for n in args.apps})
    if any(not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9._+-]*',n) for n in names):ap.error('application names must be simple names')
    def run(*cmd,**kw):subprocess.run([str(c) for c in cmd],check=True,cwd=ROOT,**kw)
    if not args.no_build:
        if 'pkg' in names:
            run(sys.executable,ROOT/'ports/pkg/build_guest.py','--target',args.target)
            if args.target=='fuse-x':
                run(sys.executable,ROOT/'rfl/tools/build_os.py',ROOT/'rfl/apps/pkg/main.rfl','--os','fuse-x','-o',ROOT/'build/pkg/pkg.fxe','--build-dir',ROOT/'build/pkg/fuse-sdk')
            names.remove('pkg')
        if args.target=='x64' and names:
            # This owner's incremental offline builder selects the supported
            # x64 C application set. Do not pretend Fuse-X FXEs are x64 code.
            sys.path.insert(0,str(ROOT/'tools/x86_64'))
            from apps import build_apps
            build_apps(build,'offline',networking=True)
            missing=[n for n in names if not (build/'apps'/f'{n}.fxe').is_file()]
            if missing:raise SystemExit('Not built for x64: '+', '.join(missing))
        elif args.target=='fuse-x':
            graphics=[n for n in names if n in GUI]
            puzzles={g['id'] for g in json.loads((ROOT/'ports/puzzles/catalog.json').read_text())}
            graphics=sorted(set(graphics)|set(names)&puzzles)
            if graphics:run(sys.executable,ROOT/'fuse-x/graphics.py','--build',build,'--only',*graphics)
            special={'rift','rfl-fm','rfl-demo','telegram','music','wavplay','qr-demo','chat','native-hello'}
            for n in names:
                if n in graphics:continue
                if n in {'cc','rcc-as','rcc-c','rcc-ld','rcc-ripple'}:
                    run(sys.executable,ROOT/'rcc-portable/package.py','--os','fuse-x','--build',build)
                elif n in special:
                    run('make','-C',ROOT/'fuse-x',f'BUILD={build}',build/'apps'/f'{n}.fxe')
                else:run(sys.executable,ROOT/'fuse-x/apps.py',n,'--build',build)
    out=ROOT/'build/pkg/catalog'/args.target;out.mkdir(parents=True,exist_ok=True)
    engine=ROOT/'build/pkg/host/pkg-engine'
    run('sh',ROOT/'ports/pkg/build.sh')
    Catalog(build,out,engine,args.target).run()
    if args.publish:run(sys.executable,ROOT/'tools/packages/publish-catalog.py',out)
if __name__=='__main__':main()
