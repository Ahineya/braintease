#!/usr/bin/env python3
"""Run real C16 data generators in a disposable Fuse-X disk, then pack assets."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
sys.path.insert(0,str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resume-levels',action='store_true',help='resume after target headers and tools were built')
    args = parser.parse_args()
    directory = PORT / 'build/generator'
    directory.mkdir(parents=True,exist_ok=True)
    tools = PORT / 'build/fuse-x'
    if not args.resume_levels:
        manifest = []
        for folder in ['dat','include','src','util']:
            manifest.append(dict(name='/nh/'+folder,directory=True))
        for path in (PORT / 'upstream/dat').iterdir():
            if path.is_file(): manifest.append(dict(name='/nh/dat/'+path.name,path=str(path)))
        for name in ['makedefs','lev_comp','dgn_comp']:
            manifest.append(dict(name='/nh/util/'+name,path=str(tools / (name+'.fxe')),mode=0o100755))
        manifest_path = directory/'manifest.json'
        manifest_path.write_text(json.dumps(manifest))
        subprocess.run(['node',str(PORT/'test_image.mjs'),str(manifest_path),str(directory/'disk.img'),'native',str(ROOT/'fuse-x/build')],cwd=ROOT,check=True)
        guest = Guest(directory,'native',{},reuse=True)
        try:
            print(guest.shell('cd /nh/util'),flush=True)
            for opt in ['p','o','z','v','d','r','q','h','s','e']:
                result = guest.shell('./makedefs -'+opt+'; echo GEN-STATUS-$?',300)
                print(result,flush=True)
                if 'GEN-STATUS-0' not in result: raise RuntimeError('makedefs -'+opt+' failed')
        finally: guest.close()
        # Target makedefs has now described the actual C16 struct layout. Compile
        # the file writers against that header, not the host bootstrap fingerprint.
        subprocess.run(['node',str(PORT/'pack.mjs'),str(directory/'disk.img'),'--headers-only'],cwd=ROOT,check=True)
        for name in ['lev_comp','dgn_comp']:
            subprocess.run([sys.executable,str(PORT/'build.py'),'fuse-x','--group',name,'--prepared'],cwd=ROOT,check=True)
    subprocess.run(['node','--input-type=module','-e',"""
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
const [disk,tools]=process.argv.slice(1);
atomicWrite(disk,installRoot(openImage(disk),['lev_comp','dgn_comp'].map(name=>({name:'/nh/util/'+name,mode:0o100755,data:fs.readFileSync(tools+'/'+name+'.fxe')}))));
""",str(directory/'disk.img'),str(tools)],cwd=ROOT,check=True)
    guest = Guest(directory,'native',{},reuse=True)
    try:
        print(guest.shell('cd /nh/dat'),flush=True)
        levels = ['bigroom','castle','endgame','gehennom','knox','medusa','mines','oracle','sokoban','tower','yendor','Arch','Barb','Caveman','Healer','Knight','Monk','Priest','Ranger','Rogue','Samurai','Tourist','Valkyrie','Wizard']
        for name in levels:
            result = guest.shell('../util/lev_comp '+name+'.des; echo GEN-STATUS-$?',300)
            print(result,flush=True)
            if 'GEN-STATUS-0' not in result: raise RuntimeError('lev_comp '+name+' failed')
        result = guest.shell('../util/dgn_comp dungeon.pdf; echo GEN-STATUS-$?',300)
        print(result,flush=True)
        if 'GEN-STATUS-0' not in result: raise RuntimeError('dgn_comp failed')
    finally: guest.close()
    subprocess.run(['node',str(PORT/'pack.mjs'),str(directory/'disk.img')],cwd=ROOT,check=True)

if __name__ == '__main__': main()
