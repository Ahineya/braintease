#!/usr/bin/env python3
"""Import ordinary WAV octets into /music in a stopped C16 guest image."""
import argparse
from contextlib import ExitStack
import fcntl
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r"""
import fs from 'node:fs';
import {openImage,writeImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
const [target,filename,manifest]=process.argv.slice(1),original=openImage(filename);
const root=target==='native'?{data:original,replace:value=>value}:fatFile(original,'ROOTFS.IMG');
const checked=checkRoot(root.data,true);
if(checkRoot(checked.image).issues.length)throw Error('filesystem repair failed');
const files=JSON.parse(manifest).map(item=>({name:'/music/'+item.name,mode:0o100644,data:fs.readFileSync(item.path)}));
const installed=installRoot(checked.image,files);
if(checkRoot(installed).issues.length)throw Error('imported filesystem failed validation');
const updated=root.replace(installed);
if(!original.equals(updated)) {
 let backup=filename+'.bak',serial=0;while(fs.existsSync(backup))backup=filename+'.bak.'+(++serial);
 writeImage(backup,original,true);fs.chmodSync(backup,0o600);atomicWrite(filename,updated);fs.chmodSync(filename,0o600);
 console.log('Imported '+files.length+' WAV file(s) into /music; backup: '+backup);
} else console.log('These music files are already installed.');
"""


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],required=True)
    parser.add_argument('--disk',type=Path,required=True)
    parser.add_argument('files',type=Path,nargs='+')
    args=parser.parse_args()
    disk=args.disk.resolve(strict=True);files=[];names=set()
    for source in args.files:
        source=source.resolve(strict=True);name=source.name
        if not name.isascii() or len(name)>30 or name in names:
            parser.error('use distinct ASCII filenames of at most 30 characters')
        names.add(name)
        if os.path.samefile(source,disk):parser.error('a source file cannot be the guest disk')
        with source.open('rb') as stream:
            head=stream.read(12)
        if len(head)!=12 or head[:4]!=b'RIFF' or head[8:]!=b'WAVE':
            parser.error(f'not a RIFF/WAVE file: {source}')
        files.append(dict(name=name,path=str(source)))
    with ExitStack() as stack:
        for path in (Path(str(disk)+'.lock'),disk):
            lock=stack.enter_context(path.open('a+b' if path!=disk else 'r+b'))
            if path!=disk:os.chmod(path,0o600)
            try:fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:raise SystemExit('Disk is in use; shut down its guest before importing') from None
        subprocess.run(['node','--input-type=module','-e',SCRIPT,args.target,str(disk),json.dumps(files)],cwd=ROOT,check=True)


if __name__=='__main__':main()
