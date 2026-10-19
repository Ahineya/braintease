import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import {buildDisk, rootImage, atomicWrite} from './disk.mjs';
import {fatFile} from './fat.mjs';
import {fat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {BLOCK, CELLS, SB, ROOT_BLOCKS, DIRECT, INDIRECT, inodeBase} from './layout.mjs';
import {checkRoot} from './fsck.mjs';
import {installRoot, growRoot, installBlock} from './install.mjs';
import {Image, openImage, writeImage} from './image.mjs';
import {rootReader} from './read-root.mjs';
const base = (image, name) => rootReader(image).lookup(name).base;
const text = (image, name) => String.fromCharCode(...rootReader(image).read(name));
const small = files => rootImage(files, 8192, 128);
function temporary(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-v3-'));
  try { fn(dir); } finally { fs.rmSync(dir, {recursive: true, force: true}); }
}
function programs(dir) {
  for (const name of ['init','hello','forkcheck','preemptcheck','iotest']) fs.writeFileSync(path.join(dir, `${name}.rxe`), name);
  fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'kernel');
  fs.writeFileSync(path.join(dir, '.mmu'), '1\n');
}
test('sparse images snapshot pages and preserve the source file across atomic replacement', () => temporary(dir => {
  assert.equal(new Image(17).subarray(0, 512).length,17);
  const image = new Image(1024 ** 3); image.writeUInt32LE(0x12345678, image.length - 4);
  const fork = image.fork(); image.writeUInt16LE(99, image.length - 4);
  assert.equal(fork.readUInt32LE(fork.length - 4), 0x12345678);
  const name = path.join(dir, 'raw'); writeImage(name, fork);
  const original = openImage(name), update = original.fork(); update.writeUInt16LE(42, 0);
  atomicWrite(name, update);
  assert.equal(original.readUInt16LE(0), 0);
  // Reusing a view of the old inode must not clone the replacement's bytes.
  writeImage(path.join(dir, 'snapshot'), original);
  assert.equal(openImage(path.join(dir, 'snapshot')).readUInt16LE(0), 0);
  assert.equal(openImage(name).readUInt16LE(0), 42);
  assert.equal(fs.statSync(name).size, 1024 ** 3);
  for (const filename of [name, path.join(dir, 'snapshot')])
    assert.ok(fs.statSync(filename).blocks * 512 < 1024 ** 2, 'sparse copies must retain holes');
  assert.throws(() => writeImage(name, openImage(name)), /in place/);
  assert.equal(openImage(name).readUInt16LE(0), 42);
}));
test('V3 directories, 32-bit free lists and double-indirect files are consistent', () => {
  const file = Buffer.alloc((DIRECT + INDIRECT + 2) * CELLS + 7, 0x61);
  const image = small([{name:'/init',data:file}]), reader = rootReader(image);
  assert.deepEqual(reader.checked.issues, []);
  assert.ok(image.readUInt32LE((reader.lookup('/init').base + 12 + (DIRECT + 1) * 2) * 2));
  assert.equal(reader.read('/init').length, file.length);
  assert.ok(image.equals(small([{name:'/init',data:file}])));
});
test('checker repairs metadata but rejects duplicate or out-of-range 32-bit pointers', () => {
  const image = small([{name:'/init',data:Buffer.from('old')}]);
  image.writeUInt16LE(1, (CELLS + SB.clean) * 2);
  image.writeUInt32LE(999, (CELLS + SB.tfree) * 2);
  assert.equal(checkRoot(image).issues.length, 2);
  assert.deepEqual(checkRoot(checkRoot(image, true).image).issues, []);
  const broken = image.fork(); broken.writeUInt32LE(0x10002, (inodeBase(1) + 12) * 2);
  assert.throws(() => checkRoot(broken, true), /outside data area/);
});
function orphan(image, name, links = 0) {
  const reader = rootReader(image), node = reader.lookup(name);
  const parent = reader.lookup(path.posix.dirname(name)), data = reader.read(path.posix.dirname(name));
  for (let off = 0; off < data.length; off += 32)
    if (data[off] === node.ino) {
      const block = parent.map.get(Math.floor(off / CELLS));
      image.writeUInt16LE(0, (block * CELLS + off % CELLS) * 2);
    }
  image.writeUInt16LE(links, (node.base + 1) * 2);
  return node;
}
test('offline repair reclaims only unreachable zero-link pipes after validating the whole graph', () => {
  const image = small([{name:'/note',mode:0o100640,data:Buffer.from('keep me')},
    {name:'/tmp/pipe',mode:0o10600,data:Buffer.alloc((DIRECT + 2) * CELLS, 0x61)}]);
  const pipe = orphan(image, '/tmp/pipe');
  // Draining an open pipe resets i_size but retains its allocated buffers.
  image.writeUInt32LE(0, (pipe.base + 4) * 2);
  const before = image.fork();
  const checked = checkRoot(image);
  assert.ok(checked.issues.some(s => s.includes(`unlinked pipe inode ${pipe.ino}`)));
  const repaired = checkRoot(image, true);
  assert.deepEqual(checkRoot(repaired.image).issues, []);
  assert.ok(!checkRoot(repaired.image).nodes.has(pipe.ino));
  assert.equal(repaired.freeBlocks - checked.freeBlocks,
    [...checked.owners.values()].filter(ino => ino === pipe.ino).length);
  assert.equal(text(repaired.image, '/note'), 'keep me');
  assert.equal(repaired.image.readUInt16LE(base(repaired.image, '/note') * 2), 0o100640);
  assert.ok(image.equals(before), 'checks and repairs must never mutate their input');
  for (const block of pipe.map.values()) assert.ok(!checkRoot(repaired.image).owners.has(block));
});
test('orphan recovery still refuses regular files, linked pipes, and conflicting block ownership', () => {
  for (const [mode, links] of [[0o100600, 0], [0o10600, 1]]) {
    const image = small([{name:'/tmp/orphan',mode,data:Buffer.from('preserve')}]);
    orphan(image, '/tmp/orphan', links);
    assert.throws(() => checkRoot(image, true), /unreachable inode/);
  }
  const image = small([{name:'/note',data:Buffer.from('saved')},
    {name:'/tmp/pipe',mode:0o10600,data:Buffer.from('buffer')}]);
  const saved = base(image, '/note'), pipe = orphan(image, '/tmp/pipe');
  image.writeUInt32LE(image.readUInt32LE((saved + 12) * 2), (pipe.base + 12) * 2);
  assert.throws(() => checkRoot(image, true), /multiply allocated/);
});
test('locked pipe recovery saves an exact backup and preserves named files and FIFOs', () => temporary(dir => {
  const root = small([{name:'/note',mode:0o100640,data:Buffer.from('saved session')},
    {name:'/tmp/named',mode:0o10600,data:Buffer.from('named pipe')},
    {name:'/tmp/anonymous',mode:0o10600,data:Buffer.from('temporary buffer')}]);
  orphan(root, '/tmp/anonymous');
  const disk = path.join(dir, 'disk.img');
  atomicWrite(disk, fat16Image([{name:'ROOTFS.IMG',data:root}],
    {totalSectors:65536,sectorsPerFat:64,sectorsPerCluster:4}));
  const original = openImage(disk), checker = fileURLToPath(new URL('./fsck.py', import.meta.url));
  const before = spawnSync('python3', [checker, disk], {encoding:'utf8'});
  assert.equal(before.status, 1);
  assert.match(before.stderr, /unlinked pipe inode/);
  assert.ok(openImage(disk).equals(original));
  const fixed = spawnSync('python3', [checker, disk, '--repair'], {encoding:'utf8'});
  assert.equal(fixed.status, 0, fixed.stderr);
  assert.match(fixed.stdout, /repaired/);
  assert.ok(openImage(disk + '.bak').equals(original));
  const repaired = fatFile(openImage(disk), 'ROOTFS.IMG').data;
  assert.deepEqual(checkRoot(repaired).issues, []);
  assert.equal(text(repaired, '/note'), 'saved session');
  assert.equal(text(repaired, '/tmp/named'), 'named pipe');
}));
test('managed devices retain old aliases, ownership and unrelated files', () => {
  const old = small([{name:'/dev/tty',mode:0o20666,device:0x201},
    {name:'/note',mode:0o100640,data:Buffer.from('saved session')}]);
  const device = {name:'/dev/tty',mode:0o20666,device:0x200,migrateDeviceFrom:0x201};
  const original = base(old, '/dev/tty'); old.writeUInt16LE(42, (original + 2) * 2);
  // Device aliases are constructed explicitly: installer hard links apply to regular files.
  let linked = installRoot(old,[{name:'/dev/saved-console',mode:0o20666,device:0x201}]);
  const reader=rootReader(linked), dev=reader.lookup('/dev'), alias=reader.lookup('/dev/saved-console');
  const directory=reader.read('/dev');
  for(let at=0;at<directory.length;at+=32) if(directory[at]===alias.ino) {
    const block=dev.map.get(Math.floor(at/CELLS));
    linked.writeUInt16LE(reader.lookup('/dev/tty').ino,(block*CELLS+at%CELLS)*2);
  }
  linked.fill(0,alias.base*2,(alias.base+64)*2);
  linked.writeUInt16LE(2,(original+1)*2);
  linked=checkRoot(linked,true).image;
  const saved=original;
  const migrated = installRoot(linked,[device]);
  assert.equal(migrated.readUInt32LE((base(migrated,'/dev/tty')+12)*2),0x200);
  assert.equal(migrated.readUInt16LE((base(migrated,'/dev/tty')+2)*2),42);
  assert.equal(migrated.readUInt32LE((saved+12)*2),0x201);
  assert.equal(text(migrated,'/note'),'saved session');
  assert.equal(installRoot(migrated,[device]),migrated);
  assert.throws(()=>installRoot(old,[{...device,migrateDeviceFrom:undefined}]),/refusing to replace/);
  const dirty=old.fork();dirty.writeUInt16LE(1,(CELLS+SB.clean)*2);
  assert.throws(()=>installRoot(dirty,[device]),/clean root/);
});
test('upgrades preserve hard-linked user executables and managed aliases', () => {
  let image = small([{name:'/bin/vi',data:Buffer.from('old')},{name:'/bin/saved',link:'/bin/vi'},
                    {name:'/etc/passwd',mode:0o100644,data:Buffer.from('custom')}]);
  const files=[{name:'/bin/levee',data:Buffer.from('new levee')},{name:'/bin/vi',link:'/bin/levee'},
               {name:'/etc/passwd',preserve:true,data:Buffer.from('default')}];
  image=installRoot(image,files);
  assert.equal(text(image,'/bin/saved'),'old');assert.equal(text(image,'/etc/passwd'),'custom');
  assert.equal(base(image,'/bin/vi'),base(image,'/bin/levee'));
  assert.notEqual(base(image,'/bin/vi'),base(image,'/bin/saved'));
  assert.equal(installRoot(image,files),image);
  assert.throws(()=>installRoot(image,[{name:'/bin/oops',link:'/bin/missing'}]),/missing.*target/);
});
test('growth preserves inode identity and named-pipe data', () => {
  const old=small([{name:'/fifo',mode:0o10600,data:Buffer.from('pipe buffer')}]);
  const grown=growRoot(old), reader=rootReader(grown);
  assert.equal(grown.length,ROOT_BLOCKS*BLOCK);
  assert.equal(base(old,'/fifo'),base(grown,'/fifo'));
  assert.equal(text(grown,'/fifo'),'pipe buffer');
  assert.deepEqual(reader.checked.issues,[]);
});
test('SDK inode expansion preserves files and hard links without enlarging the disk', () => {
  const old=rootImage([{name:'/saved',mode:0o100640,data:Buffer.from('user data')},
    {name:'/alias',link:'/saved'}],4096,1024);
  const saved=rootReader(old).lookup('/saved').ino;
  const grown=growRoot(old,4096,2048), reader=rootReader(grown);
  assert.equal(grown.length,old.length);
  assert.equal(reader.lookup('/saved').ino,saved);
  assert.equal(reader.lookup('/alias').ino,saved);
  assert.equal(text(grown,'/saved'),'user data');
  assert.equal(grown.readUInt16LE(reader.lookup('/saved').base*2),0o100640);
  assert.equal((reader.checked.isize-2)*reader.checked.format.inodes,2048);
  assert.equal(growRoot(grown,4096,2048),grown);
  assert.deepEqual(reader.checked.issues,[]);
});
test('real V2 conversion preserves native cells, inode numbers, hard links, modes and devices', () => {
  const old=zlib.gunzipSync(fs.readFileSync(new URL('./fixtures/c16-v2.img.gz',import.meta.url)));
  const before=Buffer.from(old), original=checkRoot(old), upgraded=growRoot(old,8192), reader=rootReader(upgraded);
  assert.deepEqual(original.issues,[]);assert.equal(original.format.version,2);
  assert.deepEqual([...reader.read('/note')],[0xabcd,0x1234,10]);
  assert.equal(reader.lookup('/note').ino,8);assert.equal(reader.lookup('/note').links,1);
  assert.equal(upgraded.readUInt16LE(reader.lookup('/note').base*2),0o100640);
  assert.equal(reader.lookup('/bin/large').ino,reader.lookup('/bin/alias').ino);
  assert.equal(reader.read('/bin/large').length,1100000);
  assert.ok(reader.read('/bin/large').every(c=>c===0x9abc));
  assert.equal(upgraded.readUInt32LE((reader.lookup('/dev/test').base+12)*2),0x123);
  assert.deepEqual(old,before);
});
test('checker and installer retain high physical blocks and sparse triple-indirect mappings', () => {
  let image=rootImage([{name:'/wide',data:Buffer.from('X')}],70000,128), reader=rootReader(image);
  const node=reader.lookup('/wide'), source=node.map.get(0), high=69999;
  image.copy(image,high*BLOCK,source*BLOCK,(source+1)*BLOCK);
  image.writeUInt32LE(high,(node.base+12)*2);
  const logical=DIRECT+INDIRECT+INDIRECT**2+3;
  let next=69990;
  installBlock(image,node.base,logical,69980,()=>next--);
  image.writeUInt16LE(0xbeef,69980*BLOCK);
  image.writeUInt32LE(logical*CELLS+1,(node.base+4)*2);
  image=checkRoot(image,true).image;
  reader=rootReader(image);
  assert.equal(reader.lookup('/wide').map.get(0),high);
  assert.equal(reader.lookup('/wide').map.get(logical),69980);
  image=installRoot(image,[{name:'/bin/tool',data:Buffer.from('tool')}]);
  assert.equal(rootReader(image).lookup('/wide').map.get(logical),69980);
  assert.deepEqual(checkRoot(image).issues,[]);
});
test('default disk is sparse, updates preserve root and no-op timestamps', () => temporary(dir => {
  programs(dir);const disk=path.join(dir,'disk.img');buildDisk(dir,disk);
  let image=openImage(disk), root=fatFile(image,'ROOTFS.IMG');
  assert.equal(root.data.length,ROOT_BLOCKS*BLOCK);
  assert.match(text(root.data,'/etc/nxlaunch.cnf'),/Terminal - \/bin\/nxterm[\s\S]*Editor - \/bin\/nxterm \/bin\/vi/);
  assert.match(text(root.data,'/etc/nxlaunch.cnf'),/Eyes - \/bin\/nxeyes\nClock - \/bin\/nxclock/);
  assert.match(text(root.data,'/etc/nxlaunch.cnf'),/Roaches - \/bin\/nxroach/);
  assert.match(text(root.data,'/etc/nxlaunch.cnf'),/Tetris - \/bin\/nxtetris/);
  let changed=root.data.fork();changed.writeUInt16LE(0x5a,200);
  atomicWrite(disk,root.replace(changed));
  const expected=fatFile(openImage(disk),'ROOTFS.IMG').data;
  fs.writeFileSync(path.join(dir,'FUZIX.SYS'),Buffer.alloc(24000,0x5e));
  assert.equal(buildDisk(dir,disk),true);
  assert.ok(fatFile(openImage(disk),'ROOTFS.IMG').data.equals(expected));
  const before=fs.statSync(disk).mtimeMs;assert.equal(buildDisk(dir,disk),false);
  assert.equal(fs.statSync(disk).mtimeMs,before);
}));
test('launcher backs up repairs, preserves notes, and refuses structural damage', () => temporary(dir => {
  programs(dir);const disk=path.join(dir,'disk.img');
  // Recovery and automatic growth do not require several full-size outer-disk
  // copies. Default geometry is covered above; keep repair backups small here.
  const files=['init','hello','forkcheck','preemptcheck','iotest'].map(name => ({
    name:name==='init'?'/init':`/bin/${name}`,data:Buffer.from(name)
  }));
  atomicWrite(disk,fat16Image([{name:'FUZIX.SYS',data:Buffer.from('kernel')},
    {name:'ROOTFS.IMG',data:rootImage(files,8192,1024)}],
    {totalSectors:65536,sectorsPerFat:64,sectorsPerCluster:4}));
  const old=openImage(disk), root=fatFile(old,'ROOTFS.IMG');
  const changed=installRoot(root.data,[{name:'/note',mode:0o100640,data:Buffer.from('saved session')}]);
  changed.writeUInt16LE(1,(CELLS+SB.clean)*2);atomicWrite(disk,root.replace(changed));
  const launcher=fileURLToPath(new URL('./launch.py',import.meta.url));
  const launch=()=>spawnSync('python3',[launcher,'--build',dir,'--disk',disk,'--vm','/unused','--update','--build-only'],{encoding:'utf8'});
  const result=launch();assert.equal(result.status,0,result.stderr);assert.match(result.stdout,/Recovered/);
  const good=fatFile(openImage(disk),'ROOTFS.IMG').data;
  assert.equal(text(good,'/note'),'saved session');assert.deepEqual(checkRoot(good).issues,[]);
  assert.ok(fs.existsSync(disk+'.bak'));const before=fs.statSync(disk).mtimeMs;
  assert.equal(launch().status,0);assert.equal(fs.statSync(disk).mtimeMs,before);
  const broken=good.fork();broken.writeUInt32LE(broken.readUInt32LE((base(good,'/init')+12)*2),(base(good,'/bin/hello')+12)*2);
  atomicWrite(disk,fatFile(openImage(disk),'ROOTFS.IMG').replace(broken));
  const backups=fs.readdirSync(dir).filter(x=>x.startsWith('disk.img.bak'));
  const refused=launch();assert.notEqual(refused.status,0);assert.match(refused.stderr,/multiply allocated/);
  assert.deepEqual(fs.readdirSync(dir).filter(x=>x.startsWith('disk.img.bak')),backups);
}));
test('outer FAT rejects cycles, inconsistent copies and resizing', () => temporary(dir => {
  programs(dir);const disk=path.join(dir,'disk.img');buildDisk(dir,disk);
  const image=openImage(disk), cyclic=image.fork();
  assert.throws(()=>fatFile(image,'ROOTFS.IMG').replace(Buffer.alloc(1)),/resize/);
  assert.throws(()=>fatFile(image.subarray(0,1000),'ROOTFS.IMG'));
  const root=(cyclic.readUInt16LE(14)+cyclic.readUInt8(16)*cyclic.readUInt16LE(22))*512;
  const first=cyclic.readUInt16LE(root+32+26);
  for(let n=0;n<2;n++)cyclic.writeUInt16LE(first,512+n*cyclic.readUInt16LE(22)*512+first*2);
  assert.throws(()=>fatFile(cyclic,'ROOTFS.IMG'),/chain/);
}));

test('ordinary install upgrades an outer V2 disk with a byte-exact backup', () => temporary(dir => {
  programs(dir);
  const legacy=zlib.gunzipSync(fs.readFileSync(new URL('./fixtures/c16-v2.img.gz',import.meta.url)));
  const disk=path.join(dir,'disk.img'), old=fat16Image([{name:'FUZIX.SYS',data:Buffer.from('old')},{name:'ROOTFS.IMG',data:legacy}]);
  fs.writeFileSync(disk,old);
  const launcher=fileURLToPath(new URL('./launch.py',import.meta.url));
  const refused=spawnSync('python3',[launcher,'--build',dir,'--disk',disk,'--vm','/unused','--','--net','off'],{encoding:'utf8'});
  assert.notEqual(refused.status,0);assert.match(refused.stderr,/C16 V2 disk needs an upgrade/);
  assert.deepEqual(fs.readFileSync(disk),old);assert.ok(!fs.existsSync(disk+'.bak'));
  assert.equal(buildDisk(dir,disk,false,true),true);
  const upgraded=fatFile(openImage(disk),'ROOTFS.IMG').data;
  assert.equal(upgraded.length,ROOT_BLOCKS*BLOCK);
  assert.deepEqual([...rootReader(upgraded).read('/note')],[0xabcd,0x1234,10]);
  assert.equal(base(upgraded,'/bin/large'),base(upgraded,'/bin/alias'));
  assert.deepEqual(fs.readFileSync(disk+'.bak'),old);
}));

test('pathname sockets survive checks, managed installs and growth without data blocks', () => {
  // Start with an empty ordinary file, then make the same disk inode a socket.
  const image = small([{name:'/tmp/service',data:Buffer.alloc(0)}]);
  const at = base(image, '/tmp/service'); image.writeUInt16LE(0o140700,at*2);
  assert.deepEqual(checkRoot(image).issues, []);
  assert.equal(rootReader(image).lookup('/tmp/service').map.size,0);
  const updated = installRoot(image,[{name:'/bin/new',data:Buffer.from('app')}]);
  const grown = growRoot(updated,16384), node = rootReader(grown).lookup('/tmp/service');
  assert.equal(node.type,0xc000);assert.equal(node.size,0);assert.equal(node.map.size,0);
  const badSize=image.fork();badSize.writeUInt32LE(1,(at+4)*2);
  assert.throws(()=>checkRoot(badSize),/invalid socket metadata/);
  const badPointer=image.fork();badPointer.writeUInt32LE(123,(at+12)*2);
  assert.throws(()=>checkRoot(badPointer),/invalid socket metadata/);
});

test('symlinks migrate copies and hard links, preserve user aliases and survive growth', () => {
  let image = small([{name:'/bin/levee',data:Buffer.from('old')},
    {name:'/bin/vi',link:'/bin/levee'},{name:'/bin/saved',link:'/bin/vi'},
    {name:'/bin/ln',data:Buffer.from('old copy')},
    {name:'/note',mode:0o100640,data:Buffer.from('keep')}]);
  const files = [{name:'/bin/levee',data:Buffer.from('new')},
    {name:'/bin/vi',symlink:'levee'},{name:'/bin/ln',symlink:'cp'},
    {name:'/dangling',symlink:'missing'},{name:'/cycle-a',symlink:'cycle-b'},
    {name:'/cycle-b',symlink:'cycle-a'}, {name:'/unicode-link',symlink:'\ud835\udf0c'}];
  image = installRoot(image, files);
  const reader = rootReader(image), link = reader.lookup('/bin/vi');
  assert.equal(link.type,0xa000);assert.equal(link.links,1);
  assert.notEqual(link.ino,reader.lookup('/bin/levee').ino);
  assert.equal(text(image,'/bin/vi'),'levee');assert.equal(text(image,'/bin/ln'),'cp');
  assert.equal(text(image,'/bin/saved'),'old');assert.equal(text(image,'/note'),'keep');
  assert.equal(installRoot(image,files),image);
  assert.deepEqual([...reader.read('/unicode-link')],[0xd835,0xdf0c]);
  const grown=growRoot(image,16384);
  assert.equal(rootReader(grown).lookup('/bin/vi').ino,link.ino);
  assert.equal(text(grown,'/bin/vi'),'levee');assert.deepEqual(checkRoot(grown).issues,[]);
  assert.equal(text(installRoot(image,[{name:'/bin/vi',data:Buffer.from('regular')}]),'/bin/levee'),'new');
  assert.throws(()=>installRoot(image,[{name:'/bin/vi/escape',data:Buffer.from('bad')}]),/not an install directory/);
  for(const target of ['', 'x'.repeat(512), 'a\0b'])
    assert.throws(()=>installRoot(image,[{name:'/bad',symlink:target}]),/invalid install symbolic/);
  const corrupt=image.fork();corrupt.writeUInt32LE(0,(link.base+4)*2);
  assert.throws(()=>checkRoot(corrupt),/invalid symbolic link/);
});
test('RVM installs executable aliases as relative guest symlinks', () => temporary(dir => {
  programs(dir);
  const names=['init','levee','rift','vi','cp','ln','mv','rfl-fm','mc','picol','tcl','tclsh','sok','sokoban','host','nslookup'];
  for(const n of names)fs.writeFileSync(path.join(dir,n+'.rxe'),n);
  fs.writeFileSync(path.join(dir,'apps.json'),JSON.stringify(names));
  const disk=path.join(dir,'disk.img');buildDisk(dir,disk);
  const root=fatFile(openImage(disk),'ROOTFS.IMG').data,reader=rootReader(root);
  for(const [alias,target] of Object.entries({vi:'rift',ln:'cp',mv:'cp',mc:'rfl-fm',tcl:'picol',tclsh:'picol',sokoban:'sok',nslookup:'host'})) {
    assert.equal(reader.lookup('/bin/'+alias).type,0xa000);
    assert.equal(text(root,'/bin/'+alias),target);
  }
}));
