#!/usr/bin/env python3
"""Offline login migration: locking, private modes, validation and preservation."""
import fcntl
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
INSTALL = ROOT / 'ports/telegram/install.py'
SETUP = r'''
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage, Image} from './ports/fuzix/tools/image.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [dir,target,variant]=process.argv.slice(1);
const config=Buffer.from('api_id=12345\napi_hash='+'0'.repeat(32)+'\n');
const session=Buffer.alloc(328);session.write('FTG1');session.writeUInt32LE(12345,4);
session[8]=2;session[10]=1;session.fill(7,12,268);
createHash('sha256').update(session.subarray(0,296)).digest().copy(session,296);
if(variant==='corrupt')session[20]^=1;
const login=[{name:'/etc/telegram.conf',mode:0o100600,data:config},
  {name:'/telegram.session',mode:0o100600,data:session}];
writeImage(dir+'/source.img',rootImage(login,1024,128),true);
if(variant==='different') {
  const other=Buffer.from(session);other[12]^=1;
  login[1]={...login[1],data:other};
}
const root=rootImage([{name:'/note',mode:0o100640,data:Buffer.from('keep my files')},
  ...(variant==='different'?login:[])],2048,128);
writeImage(dir+'/disk.img',target==='native'?root:fat16Image([{name:'ROOTFS.IMG',data:root}],
  {totalSectors:65536,sectorsPerFat:64,sectorsPerCluster:4,imageFactory:n=>new Image(n)}),true);
fs.writeFileSync(dir+'/telegram',Buffer.from(target==='native'?'FUSEX001dummy':'RXE1dummy'));
'''
VERIFY = r'''
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {CELLS,BLOCK} from './ports/fuzix/tools/layout.mjs';
const [dir,target]=process.argv.slice(1),image=openImage(dir+'/disk.img');
const checked=checkRoot(target==='native'?image:fatFile(image,'ROOTFS.IMG').data);
assert.equal(checked.issues.length,0);
function get(path) {
  let node=checked.nodes.get(1);
  function cell(at) {return checked.image.readUInt16LE(node.map.get(Math.floor(at/CELLS))*BLOCK+(at%CELLS)*2);}
  for(const part of path.split('/').filter(Boolean)) {
    let child;
    for(let at=0;at<node.size;at+=32) {
      let name='';for(let j=1;j<=30&&cell(at+j);j++)name+=String.fromCharCode(cell(at+j));
      if(name===part){child=checked.nodes.get(cell(at));break;}
    }
    assert.ok(child);node=child;
  }
  return {mode:checked.image.readUInt16LE(node.base*2),data:Buffer.from(Array.from({length:node.size},(_,i)=>cell(i)))};
}
assert.equal(get('/etc/telegram.conf').mode,0o100600);
assert.equal(get('/telegram.session').mode,0o100600);
assert.equal(get('/telegram.session').data.length,328);
assert.equal(get('/note').data.toString(),'keep my files');
assert.equal(get('/note').mode,0o100640);
assert.equal(get('/bin/telegram').mode,0o100755);
assert.ok(get('/bin/telegram').data.equals(fs.readFileSync(dir+'/telegram')));
'''


class InstallTests(unittest.TestCase):
    def setup_image(self, directory, target, variant='valid'):
        subprocess.run(['node', '--input-type=module', '-e', SETUP, str(directory), target, variant],
                       cwd=ROOT, check=True, capture_output=True)

    def install(self, directory, target):
        return subprocess.run([sys.executable, str(INSTALL), '--source-image', str(directory / 'source.img'),
                               '--target', target, '--disk', str(directory / 'disk.img'),
                               '--executable', str(directory / 'telegram')], cwd=ROOT, capture_output=True, text=True)

    def test_migration_and_locks(self):
        for target in ('native', 'ripple'):
            with self.subTest(target=target), tempfile.TemporaryDirectory(prefix='tg-install-') as temporary:
                directory = Path(temporary)
                self.setup_image(directory, target)
                for image in ('source.img', 'disk.img'):
                    for suffix in ('', '.lock'):
                        with (directory / (image + suffix)).open('a+b') as lock:
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            result = self.install(directory, target)
                            self.assertNotEqual(result.returncode, 0)
                            self.assertIn('Disk is in use', result.stderr)
                            self.assertFalse(list(directory.glob('disk.img.bak*')))
                result = self.install(directory, target)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run(['node', '--input-type=module', '-e', VERIFY, str(directory), target],
                               cwd=ROOT, check=True, capture_output=True)
                backups = list(directory.glob('disk.img.bak*'))
                self.assertEqual(len(backups), 1)
                for image in [directory / 'disk.img', *backups]:
                    self.assertEqual(image.stat().st_mode & 0o777, 0o600)
                result = self.install(directory, target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(list(directory.glob('disk.img.bak*')), backups)

    def test_invalid_login_does_not_change_destination(self):
        for variant in ('corrupt', 'different'):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory(prefix='tg-install-') as temporary:
                directory = Path(temporary)
                self.setup_image(directory, 'native', variant)
                before = (directory / 'disk.img').read_bytes()
                result = self.install(directory, 'native')
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((directory / 'disk.img').read_bytes(), before)
                self.assertFalse(list(directory.glob('disk.img.bak*')))


if __name__ == '__main__':
    unittest.main()
