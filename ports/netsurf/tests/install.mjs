// Install a maintained browser, retain user files, and refuse an open image.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawn,spawnSync} from 'node:child_process';
import {once} from 'node:events';
import {rootImage} from '../../fuzix/tools/disk.mjs';
import {Image,writeImage} from '../../fuzix/tools/image.mjs';
import {checkRoot} from '../../fuzix/tools/fsck.mjs';
import {fatFile} from '../../fuzix/tools/fat.mjs';
import {fat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';

const directory=fs.mkdtempSync(path.join(os.tmpdir(),'netsurf-install-'));
const run=args=>spawnSync('node',['ports/netsurf/install.mjs',...args],{encoding:'utf8',env:{...process.env,RIPPLE_IMAGE_BACKUPS:'1'}});
function file(root,name) {
  const checked=checkRoot(root),{nodes}=checked;
  assert.deepEqual(checked.issues,[]);
  const cell=(node,at)=>root.readUInt16LE((node.map.get(Math.floor(at/1024))*1024+at%1024)*2);
  let node=nodes.get(1);
  for(const component of name.split('/').filter(Boolean)) {
    let found;
    for(let at=0;at<node.size;at+=32) {
      let label='';
      for(let j=1;j<=30&&cell(node,at+j);j++)label+=String.fromCharCode(cell(node,at+j));
      if(label===component){found=nodes.get(cell(node,at));break;}
    }
    assert.ok(found,name);node=found;
  }
  return Buffer.from(Array.from({length:node.size},(_,i)=>cell(node,i)));
}
try {
  const native=path.join(directory,'browser.fxe'),ripple=path.join(directory,'browser.rxe');
  fs.writeFileSync(native,'FUSEX001maintained browser');fs.writeFileSync(ripple,'RXE1maintained browser');
  for(const [target,program,wrapped] of [['native',native,false],['ripple',ripple,true]]) {
    const root=rootImage([{name:'/my-note',data:Buffer.from('USER DATA')},
      {name:'/etc/ssl/cert.pem',data:Buffer.from('MY TRUST FILE')}],16384,256);
    const original=wrapped?fat16Image([{name:'FUZIX.SYS',data:Buffer.from('kernel')},{name:'ROOTFS.IMG',data:root}],
      {totalSectors:131072,sectorsPerFat:256,sectorsPerCluster:4}):root;
    const input=path.join(directory,target+'.img'),copy=path.join(directory,target+'-copy.img');
    writeImage(input,original);
    let result=run([target,input,copy,program,program]);
    assert.equal(result.status,0,result.stderr);
    assert.ok(original.equals(fs.readFileSync(input)),'copy install changed the source');
    result=run(['--in-place',target,input,program,program]);
    assert.equal(result.status,0,result.stderr);
    assert.ok(original.equals(fs.readFileSync(input+'.bak')),'missing original backup');
    const image=Image.from(fs.readFileSync(input)),installed=wrapped?fatFile(image,'ROOTFS.IMG').data:image;
    assert.equal(file(installed,'/my-note').toString(),'USER DATA');
    assert.equal(file(installed,'/etc/ssl/cert.pem').toString(),'MY TRUST FILE');
    assert.ok(file(installed,'/bin/netsurf').equals(fs.readFileSync(program)));
    assert.ok(file(installed,'/bin/htget').equals(fs.readFileSync(program)));
    assert.ok(file(installed,'/usr/share/netsurf/images.html').length);
    result=run([target,input,copy,program,program]);
    assert.notEqual(result.status,0,'existing output overwritten');
    const child=spawn('python3',['-u','-c',
      'import sys,time; f=open(sys.argv[1],"rb"); print("READY",flush=True); time.sleep(30)',input],{stdio:['ignore','pipe','pipe']});
    try {
      await once(child.stdout,'data');
      result=run(['--in-place',target,input,program,program]);
      assert.notEqual(result.status,0,'open image was overwritten');
      assert.match(result.stderr,/image is open/);
      assert.ok(image.equals(fs.readFileSync(input)),'refused install changed the image');
    } finally {child.kill();await once(child,'exit');}
  }
  console.log('NetSurf installer: native/Ripple copies, atomic upgrades, backups, user files, trust preservation and open-image refusal passed');
} finally {fs.rmSync(directory,{recursive:true,force:true});}
