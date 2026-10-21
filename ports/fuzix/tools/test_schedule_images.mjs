// Exercise both actual image producers using fake artifacts and private disks.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {buildDisk} from './disk.mjs';
import {fatFile} from './fat.mjs';
import {openImage} from './image.mjs';
import {rootReader} from './read-root.mjs';
import {installRoot} from './install.mjs';
import {scheduleFiles} from './schedule.mjs';
const root=fileURLToPath(new URL('../../../',import.meta.url));
const work=fs.mkdtempSync(path.join(os.tmpdir(),'schedule-images-'));
const text=(image,name)=>String.fromCharCode(...rootReader(image).read(name));
function verify(image) {
  const reader=rootReader(image),alias=reader.lookup('/bin/crond');
  assert.equal(image.readUInt16LE(alias.base*2)&0xf000,0xa000);
  assert.equal(text(image,'/bin/crond'),'cron');
  assert.match(text(image,'/etc/services/cron.ini'),/program=\/bin\/cron/);
  assert.ok(text(image,'/etc/crontab').split('\n').every(line=>!line||line[0]==='#'));
  for(const name of ['','/jobs','/running','/output']) {
    const node=reader.lookup('/var/spool/at'+name);
    assert.equal(image.readUInt16LE(node.base*2),0o40700);
  }
  const changed=installRoot(image,[{name:'/etc/crontab',mode:0o100600,data:Buffer.from('* * * * * echo saved\n')}]);
  const again=installRoot(changed,scheduleFiles());
  assert.equal(text(again,'/etc/crontab'),'* * * * * echo saved\n');
  assert.ok(!text(again,'/etc/boot/services.ini').includes('enabled=cron'));
}
try {
  const apps=path.join(work,'apps');fs.mkdirSync(apps);
  const names=['cron','crond','at','atrun','tree','getopt','cal','native-hello'];
  for(const name of names)fs.writeFileSync(path.join(apps,name+'.fxe'),name);
  fs.writeFileSync(path.join(work,'application-report.json'),JSON.stringify(names.map(name=>({name,status:'built'}))));
  const native=path.join(work,'native.img');
  const made=spawnSync('node',['fuse-x/image.mjs','create',native,'--apps',apps],{cwd:root,encoding:'utf8'});
  assert.equal(made.status,0,made.stderr);
  // These tests explicitly add a disabled profile for the assertion above;
  // scheduling itself must not produce a boot activation file.
  const disabled={name:'/etc/boot/services.ini',mode:0o100600,data:Buffer.from('[services]\nenabled=\n')};
  verify(installRoot(openImage(native),[disabled]));
  fs.writeFileSync(path.join(work,'apps.json'),JSON.stringify(['init',...names.filter(n=>n!=='native-hello')]));
  for(const name of ['init',...names])fs.writeFileSync(path.join(work,name+'.rxe'),name);
  fs.writeFileSync(path.join(work,'FUZIX.SYS'),'kernel');fs.writeFileSync(path.join(work,'.mmu'),'1\n');
  const ripple=path.join(work,'ripple.img');buildDisk(work,ripple);
  verify(installRoot(fatFile(openImage(ripple),'ROOTFS.IMG').data,[disabled]));
  fs.writeFileSync(path.join(apps,'manifest.json'),JSON.stringify(names.filter(n=>n!=='native-hello')));
  const x64=path.join(work,'x64.img');
  const xmade=spawnSync('node',['tools/x86_64/riplix/root.mjs',x64,apps],{cwd:root,encoding:'utf8'});
  assert.equal(xmade.status,0,xmade.stderr);
  verify(installRoot(openImage(x64),[disabled]));
  console.log('Scheduling image tests: Fuse-X, RVM and x64; crond aliases, private queues, disabled defaults, preserved configuration.');
} finally {fs.rmSync(work,{recursive:true,force:true});}
