// Check normal image producers, without updating a user's disk.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {buildDisk} from '../../fuzix/tools/disk.mjs';
import {fatFile} from '../../fuzix/tools/fat.mjs';
import {openImage} from '../../fuzix/tools/image.mjs';
import {rootReader} from '../../fuzix/tools/read-root.mjs';
const root=fileURLToPath(new URL('../../../',import.meta.url));
const work=fs.mkdtempSync(path.join(os.tmpdir(),'pkg-aliases-'));
try {
  const stage=path.join(work,'apps');fs.mkdirSync(stage);
  for(const name of ['test','native-hello'])fs.writeFileSync(path.join(stage,name+'.fxe'),name);
  fs.writeFileSync(path.join(work,'application-report.json'),JSON.stringify([{name:'test',status:'built'}]));
  const native=path.join(work,'native.img');
  const made=spawnSync('node',['fuse-x/image.mjs','create',native,'--apps',stage],{cwd:root,encoding:'utf8'});
  assert.equal(made.status,0,made.stderr);
  const verify=image=>{
    const reader=rootReader(image),alias=reader.lookup('/bin/[');
    assert.equal(image.readUInt16LE(alias.base*2)&0xf000,0xa000);
    assert.equal(String.fromCharCode(...reader.read('/bin/[')),'test');
  };
  verify(openImage(native));
  fs.writeFileSync(path.join(work,'apps.json'),JSON.stringify(['init','test']));
  for(const name of ['init','test'])fs.writeFileSync(path.join(work,name+'.rxe'),name);
  fs.writeFileSync(path.join(work,'FUZIX.SYS'),'kernel');
  fs.writeFileSync(path.join(work,'.mmu'),'1\n');
  const ripple=path.join(work,'ripple.img');buildDisk(work,ripple);
  verify(fatFile(openImage(ripple),'ROOTFS.IMG').data);
  console.log('Fuse-X and Ripple seed images install [ as an alias for test.');
} finally {
  fs.rmSync(work,{recursive:true,force:true});
}
