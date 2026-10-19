#!/usr/bin/env python3
"""Offline music import retains private files, locks and exact octets."""
import fcntl
from pathlib import Path
import subprocess
import sys
import tempfile
from test_wav import ROOT,fixture

SETUP=r"""
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
writeImage(process.argv[1],rootImage([{name:'/telegram.session',mode:0o100600,data:Buffer.from('private fixture')}],4096,128),true);
"""
VERIFY=r"""
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {CELLS,BLOCK} from './ports/fuzix/tools/layout.mjs';
const c=checkRoot(openImage(process.argv[1]));assert.equal(c.issues.length,0);
function get(path) {
 let n=c.nodes.get(1);
 const cell=at=>c.image.readUInt16LE(n.map.get(Math.floor(at/CELLS))*BLOCK+(at%CELLS)*2);
 for(const part of path.split('/').filter(Boolean)) {
  let found;for(let i=0;i<n.size;i+=32) {
   let name='';for(let j=1;j<=30&&cell(i+j);j++)name+=String.fromCharCode(cell(i+j));
   if(name===part){found=c.nodes.get(cell(i));break;}
  }assert.ok(found);n=found;
 }return {data:Buffer.from(Array.from({length:n.size},(_,i)=>cell(i))),mode:c.image.readUInt16LE(n.base*2)};
}
assert.equal(get('/telegram.session').mode,0o100600);
assert.equal(get('/telegram.session').data.toString(),'private fixture');
assert.ok(get('/music/track.wav').data.equals(fs.readFileSync(process.argv[2])));
"""


def main():
    with tempfile.TemporaryDirectory(prefix='music-import-') as temporary:
        directory=Path(temporary);disk=directory/'root.img';source=directory/'track.wav'
        source.write_bytes(fixture(frames=123)[0])
        subprocess.run(['node','--input-type=module','-e',SETUP,str(disk)],cwd=ROOT,check=True)
        command=[sys.executable,str(ROOT/'ports/music/import.py'),'--target','native','--disk',str(disk),str(source)]
        for path in (disk,Path(str(disk)+'.lock')):
            with path.open('a+b') as lock:
                fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
                result=subprocess.run(command,capture_output=True,text=True)
                assert result.returncode and 'Disk is in use' in result.stderr
                assert not list(directory.glob('*.bak*'))
        subprocess.run(command,check=True)
        subprocess.run(command,check=True)
        assert len(list(directory.glob('*.bak*')))==1
        subprocess.run(['node','--input-type=module','-e',VERIFY,str(disk),str(source)],cwd=ROOT,check=True)
        assert disk.stat().st_mode&0o777==0o600
    print('Music import: exact octets, private-file preservation, both locks and idempotency passed')


if __name__=='__main__':main()
