#!/usr/bin/env node
// Reuse the canonical C16 image builder; never update a running Ripple disk.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage, atomicWrite} from '../ports/fuzix/tools/disk.mjs';
import {fatFile} from '../ports/fuzix/tools/fat.mjs';
import {openImage, writeImage} from '../ports/fuzix/tools/image.mjs';
import {ROOT_BLOCKS} from '../ports/fuzix/tools/layout.mjs';
import {installRoot, growRoot} from '../ports/fuzix/tools/install.mjs';
import {manualFiles} from '../ports/fuzix/tools/manuals.mjs';
import {sokobanFiles} from '../ports/fuzix/tools/sokoban.mjs';
import {commanderFiles, riftFiles} from '../rfl/os/files.mjs';
import {lynxFiles} from '../ports/lynx/files.mjs';

// C16 V3 shares the 1 GiB default and 32-bit block references with Ripple.

const applications = [['/init','init'],['/bin/sh','sh'],['/bin/fxsh','sh'],['/bin/hello','hello'],['/bin/check','check']]
  .map(([name, id]) => ({name, mode: 0o100755, data: Buffer.from(`FXN1:${id}:1\n`)}));

try {
  const args=process.argv.slice(2);
  const bootstrap=args.indexOf('--bootstrap-shell');
  const useBootstrap=bootstrap!==-1;
  if(useBootstrap) args.splice(bootstrap,1);
  let appDirectory;
  const option=args.indexOf('--apps');
  if(option!==-1) {
    if(!args[option+1]) throw Error('--apps requires a directory');
    appDirectory=args[option+1];args.splice(option,2);
  }
  const [command, source, destination, extra, ...rest] = args;
  if (appDirectory) {
    if(!['create','install'].includes(command)) throw Error('--apps is only supported with create/install');
    const report=path.join(appDirectory,'../application-report.json');
    const names=fs.existsSync(report)
      ? ['native-hello',...JSON.parse(fs.readFileSync(report,'utf8')).filter(a=>a.status==='built').map(a=>a.name)]
      : ['native-hello','cat','ls','cp','sh'];
    for(const name of ['rfl-demo','rfl-fm','rift']) {
      if(fs.existsSync(path.join(appDirectory,`${name}.fxe`))) names.push(name);
    }
    for(const name of ['gterm','gxcheck','nano-X','nanowm','nxlaunch','nxterm','nxclock','nxeyes','nxcheck','nxhostcheck','startx','nxdraw']) {
      if(fs.existsSync(path.join(appDirectory,`${name}.fxe`))) names.push(name);
    }
    for(const name of [...new Set(names)].sort()) {
      if(!/^[a-zA-Z0-9-]+$/.test(name)) throw Error('invalid application name in build report');
      if(!fs.existsSync(path.join(appDirectory,`${name}.fxe`))) throw Error(`missing built application: ${name}`);
      if(name==='sh') {
        if(useBootstrap) continue;
        applications.splice(applications.findIndex(a=>a.name==='/bin/sh'),1);
      }
      applications.push({name:`/bin/${name}`,mode:0o100755,data:fs.readFileSync(`${appDirectory}/${name}.fxe`)});
      if(name==='lynx') applications.push(...lynxFiles());
      if(name==='rfl-fm') applications.push({name:'/bin/mc',link:'/bin/rfl-fm'});
    }
  }
  if(['create','install'].includes(command)) {
    applications.push(...commanderFiles(), ...riftFiles());
    applications.push({name:'/mnt/host',directory:true,mode:0o40755});
    if(appDirectory && fs.existsSync(path.join(appDirectory,'nano-X.fxe')))
      applications.push({name:'/etc/nxlaunch.cnf',mode:0o100644,preserve:true,
        data:Buffer.from('Terminal - /bin/nxterm\nMac-term - /bin/gterm\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nDraw - /bin/nxdraw\n')});
    applications.push({name:'/dev/tty',mode:0o20666,device:0x200,data:Buffer.alloc(0)});
    applications.push({name:'/dev/gfx',mode:0o20666,device:0x400,data:Buffer.alloc(0)});
    for(let i=0;i<4;i++) applications.push(
      {name:`/dev/ptyp${i}`,mode:0o20600,device:0x300+i,data:Buffer.alloc(0)},
      {name:`/dev/ttyp${i}`,mode:0o20600,device:0x202+i,data:Buffer.alloc(0)});
    for(const [name,device] of [['null',0x100],['zero',0x102],['proc',0x103]])
      applications.push({name:`/dev/${name}`,mode:0o20666,device,data:Buffer.alloc(0)});
    for(const [name,data] of [
      ['/etc/passwd','root:x:0:0:root:/:/bin/sh\n'],['/etc/group','root:x:0:root\n'],
      ['/etc/mtab','/dev/root / fuzix rw 0 0\n'],
      ['/etc/resolv.conf','nameserver 192.168.64.1\n'],
      ['/etc/hosts','127.0.0.1 localhost\n192.168.64.250 fuse-x\n192.168.64.1 host\n'],
      ['/etc/termcap','ansi|vt100|xterm|xterm-256color|fuse-x:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n']])
      applications.push({name,mode:0o100644,preserve:true,data:Buffer.from(data)});
    if(appDirectory) {
      applications.push(...manualFiles(),...sokobanFiles());
      for(const name of ['makeactions','makemacros'])
        applications.push({name:`/usr/lib/${name}`,mode:0o100644,preserve:true,
          data:fs.readFileSync(new URL(`../ports/fuzix/upstream/Applications/MWC/cmd/${name}`,import.meta.url))});
    }
  }
  if (rest.length || !source || !['create','install','put','extract'].includes(command) ||
      (command==='put' ? (!destination || !extra || !destination.startsWith('/')) :
       command === 'extract' ? (!destination || extra) : (destination !== undefined))) {
    throw Error('usage: image.mjs create ROOT [--apps DIR] | install ROOT [--apps DIR] | put ROOT /guest/path HOSTFILE | extract RIPPLE-DISK ROOT');
  }
  if(command==='put') applications.splice(0,applications.length,{name:destination,mode:0o100755,data:fs.readFileSync(extra)});
  if (command === 'install' || command==='put') {
    const original = openImage(source), updated = installRoot(
      growRoot(original, ROOT_BLOCKS), applications);
    if (!original.equals(updated)) {
      let backup = `${source}.bak`, serial = 0;
      while (fs.existsSync(backup)) backup = `${source}.bak.${++serial}`;
      writeImage(backup, original, true);
      atomicWrite(source, updated);
      console.log(`Installed native programs; previous image: ${backup}`);
    }
    process.exit(0);
  }
  const output = command === 'create' ? source : destination;
  const image = command === 'create'
    ? rootImage(applications, ROOT_BLOCKS, 1024)
    : growRoot(fatFile(openImage(source), 'ROOTFS.IMG').data, ROOT_BLOCKS);
  writeImage(output, image, true);
  console.log(`Created ${output}`);
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
