import assert from 'node:assert/strict';
import test from 'node:test';
import {rootImage} from './disk.mjs';
import {installRoot} from './install.mjs';
import {rootHomeFiles} from './home.mjs';
import {rootReader} from './read-root.mjs';

const file=(name,text,mode=0o100600)=>({name,mode,data:Buffer.from(text)});
const small=files=>rootImage(files,4096,128);
const text=(reader,name)=>String.fromCharCode(...reader.read(name));
const mode=(reader,name)=>reader.checked.image.readUInt16LE(reader.lookup(name).base*2);

test('home upgrade retains private file inodes, links, Unicode cells and settings',()=>{
  const original=small([
    file('/etc/passwd','root:x:0:0:Root User:/:/bin/sh\nother:x:7:8:Other:/home/other:/bin/sh\n',0o100644),
    file('/.shrc',"PS1='my prompt> '\nexport PS1\n"),
    {name:'/.riftrc',mode:0o100600,data:[0x41,0x301,0x4e2d]},
    {name:'/keep-options',link:'/.riftrc'},
    file('/.config/rift/config.ini','run settings'),
    file('/.config/mc/config.ini','theme settings'),
    file('/.chat/account.json','private chat fixture'),
    file('/.mux/main','saved panes'), file('/.musicrc','saved playlist'),
    file('/etc/telegram.conf','private app fixture'),
    {name:'/telegram.session',mode:0o100600,data:[0,255,1,128]},
    file('/my-work','leave ordinary root files alone',0o100640),
  ]);
  const before=original.fork(),old=rootReader(original);
  const image=installRoot(original,rootHomeFiles()),reader=rootReader(image);
  assert.deepEqual(reader.checked.issues,[]);
  assert.ok(original.equals(before),'migration must not mutate its input');
  assert.equal(text(reader,'/etc/passwd'),'root:x:0:0:Root User:/root:/bin/sh\nother:x:7:8:Other:/home/other:/bin/sh\n');
  for(const [source,destination] of [
    ['/.shrc','/root/.shrc'],['/.riftrc','/root/.riftrc'],
    ['/.config','/root/.config'],['/.config/rift/config.ini','/root/.config/rift/config.ini'],
    ['/.chat/account.json','/root/.chat/account.json'],['/.mux/main','/root/.mux/main'],
    ['/.musicrc','/root/.musicrc'],['/etc/telegram.conf','/root/.telegram.conf'],
    ['/telegram.session','/root/.telegram.session'],
  ]) {
    assert.equal(reader.lookup(destination).ino,old.lookup(source).ino);
    if(old.lookup(source).type!==0x4000)
      assert.deepEqual(reader.read(destination),old.read(source));
    assert.equal(mode(reader,destination),mode(old,source));
    assert.throws(()=>reader.lookup(source),/missing/);
  }
  assert.equal(reader.lookup('/keep-options').ino,reader.lookup('/root/.riftrc').ino);
  assert.equal(reader.lookup('/keep-options').links,2);
  assert.equal(mode(reader,'/root'),0o40700);
  assert.equal(text(reader,'/my-work'),'leave ordinary root files alone');
  assert.strictEqual(installRoot(image,rootHomeFiles()),image,'repeat upgrades must be a no-op');
});

test('home upgrade merges non-overlapping directories and deduplicates equal files',()=>{
  const original=small([file('/.config/rift/config.ini','editor'),
    file('/root/.config/mc/config.ini','commander'),
    file('/.config/common','same'),file('/root/.config/common','same'),
    {name:'/.config/link',symlink:'../outside'},
    file('/outside','untouched'),file('/.mux/a','one'),file('/root/.mux/b','two')]);
  const image=installRoot(original,rootHomeFiles()),reader=rootReader(image);
  assert.deepEqual(reader.checked.issues,[]);
  assert.equal(text(reader,'/root/.config/rift/config.ini'),'editor');
  assert.equal(text(reader,'/root/.config/mc/config.ini'),'commander');
  assert.equal(text(reader,'/root/.config/common'),'same');
  assert.equal(text(reader,'/root/.config/link'),'../outside');
  assert.equal(text(reader,'/outside'),'untouched');
  assert.equal(text(reader,'/root/.mux/a'),'one');
  assert.equal(text(reader,'/root/.mux/b'),'two');
  assert.throws(()=>reader.lookup('/.config'),/missing/);
  assert.strictEqual(installRoot(image,rootHomeFiles()),image);
});

test('conflicting settings or non-directory destinations abort without touching input',()=>{
  for(const target of [file('/root/.config/rift/settings','new'),
    {name:'/root/.config',symlink:'/outside'}]) {
    const original=small([file('/.config/rift/settings','old'),target,file('/outside/settings','outside')]);
    const before=original.fork();
    assert.throws(()=>installRoot(original,rootHomeFiles()),/migration conflict/);
    assert.ok(original.equals(before));
    assert.equal(text(rootReader(original),'/.config/rift/settings'),'old');
  }
});

test('fresh homes get passwd defaults; deliberate account customizations survive',()=>{
  const fresh=rootReader(installRoot(small([]),rootHomeFiles()));
  assert.equal(text(fresh,'/etc/passwd'),'root:x:0:0:root:/root:/bin/sh\n');
  const passwd='root:custom:0:4:My name:/home/me:/bin/custom\n';
  const updated=rootReader(installRoot(small([file('/etc/passwd',passwd,0o100644)]),rootHomeFiles()));
  assert.equal(text(updated,'/etc/passwd'),passwd);
});
