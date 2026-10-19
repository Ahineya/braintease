#!/usr/bin/env node
import {ptyDevices} from '../ports/fuzix/tools/pty.mjs';
import {kernelCredits} from '../ports/fuzix/tools/credits.mjs';
// Reuse the canonical C16 image builder; never update a running Ripple disk.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage, atomicWrite} from '../ports/fuzix/tools/disk.mjs';
import {fatFile} from '../ports/fuzix/tools/fat.mjs';
import {backupImage, pruneImageBackups} from '../ports/fuzix/tools/image-backup.mjs';
import {openImage, writeImage} from '../ports/fuzix/tools/image.mjs';
import {ROOT_BLOCKS} from '../ports/fuzix/tools/layout.mjs';
import {rootHomeFiles} from '../ports/fuzix/tools/home.mjs';
import {installRoot, growRoot} from '../ports/fuzix/tools/install.mjs';
import {manualFiles} from '../ports/fuzix/tools/manuals.mjs';
import {sokobanFiles} from '../ports/fuzix/tools/sokoban.mjs';
import {commanderFiles, riftFiles} from '../rfl/os/files.mjs';
import {lynxFiles} from '../ports/lynx/files.mjs';
import {tlsFiles} from '../ports/tls/files.mjs';
import {fuseDEFiles} from '../ports/fusede/files.mjs';
import {kolfFiles} from '../ports/kolf/files.mjs';
import {klotskiFiles} from '../ports/klotski/files.mjs';
import {compilerFiles} from '../rcc-portable/files.mjs';
import {unicodeFiles} from '../libraries/unicode/files.mjs';
import {basicFiles} from '../ports/basic/files.mjs';
import {netsurfFiles} from '../ports/netsurf/files.mjs';

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
  const compiler = appDirectory ? compilerFiles(path.join(appDirectory, '../compiler')) : [];
  if (appDirectory) {
    if(!['create','install'].includes(command)) throw Error('--apps is only supported with create/install');
    const report=path.join(appDirectory,'../application-report.json');
    const names=fs.existsSync(report)
      ? ['native-hello',...JSON.parse(fs.readFileSync(report,'utf8')).filter(a=>a.status==='built').map(a=>a.name)]
      : ['native-hello','cat','ls','cp','sh'];
    for(const name of ['rfl-demo','rfl-fm','rift','qr-demo','telegram','music','wavplay','chat']) {
      if(fs.existsSync(path.join(appDirectory,`${name}.fxe`))) names.push(name);
    }
    for(const name of ['gterm','gxcheck','nano-X','nanowm','nxlaunch','nxterm','nxclock','nxeyes','nxmsg','nxselect','nxcalc','nxtetris','nxview','kolf','klotski','nxcheck','nxhostcheck','startx','nxdraw','fdeshowcase','fdectl']) {
      if(fs.existsSync(path.join(appDirectory,`${name}.fxe`))) names.push(name);
    }
    const aliases = {vi:'rift',mc:'rfl-fm',mv:'cp',ln:'cp',tcl:'picol',
                     tclsh:'picol',sokoban:'sok',nslookup:'host'};
    const selected = new Set(names);
    // Staged application sets often use symlinks into the original build.
    // Carry their SDK libraries too, even when the stage has no sibling lib/.
    const libraryDirectories=new Set([path.join(appDirectory,'../lib')]);
    for(const name of selected) {
      const executable=path.join(appDirectory,`${name}.fxe`);
      if(fs.existsSync(executable))
        libraryDirectories.add(path.resolve(path.dirname(fs.realpathSync(executable)),'../lib'));
    }
    const installedLibraries=new Set();
    for(const libraryDirectory of libraryDirectories) {
      if(!fs.existsSync(libraryDirectory)) continue;
      for(const name of fs.readdirSync(libraryDirectory).sort()) {
        if(!/^[a-zA-Z0-9._-]+\.fxl\.[0-9]+$/.test(name)) continue;
        if(installedLibraries.has(name)) continue;
        installedLibraries.add(name);
        applications.push({name:`/lib/fuse-x/${name}`,mode:0o100644,
                           data:fs.readFileSync(path.join(libraryDirectory,name))});
      }
    }
    for(const name of [...selected].sort()) {
      if(!/^[a-zA-Z0-9-]+$/.test(name)) throw Error('invalid application name in build report');
      if(!fs.existsSync(path.join(appDirectory,`${name}.fxe`))) throw Error(`missing built application: ${name}`);
      if(name==='sh') {
        if(useBootstrap) continue;
        applications.splice(applications.findIndex(a=>a.name==='/bin/sh'),1);
      }
      if(aliases[name]) {
        if(!selected.has(aliases[name])) throw Error(`missing executable alias target: ${aliases[name]}`);
        applications.push({name:`/bin/${name}`,symlink:aliases[name]});
        continue;
      }
      applications.push({name:`/bin/${name}`,mode:0o100755,data:fs.readFileSync(`${appDirectory}/${name}.fxe`)});
      if(name==='lynx') applications.push(...lynxFiles());
      if(name==='netsurf') applications.push(...netsurfFiles());
      if(name==='basic') applications.push(...basicFiles());
      if(name==='kolf') applications.push(...kolfFiles());
      if(name==='klotski') applications.push(...klotskiFiles());
      if((name==='htget'||name==='lynx'||name==='chat'||name==='netsurf')&&!applications.some(f=>f.name==='/etc/ssl/cert.pem')) applications.push(...tlsFiles());
      if(name==='fdeshowcase') {
        // Integration fixtures stage executable symlinks separately from data.
        const local=path.resolve(appDirectory,'../fusede');
        const assets=fs.existsSync(path.join(local,'theme.fdi')) ? local :
          path.resolve(path.dirname(fs.realpathSync(path.join(appDirectory,'fdeshowcase.fxe'))),'../fusede');
        applications.push(...fuseDEFiles(assets));
      }
      if(name==='rfl-fm') applications.push({name:'/bin/mc',symlink:'rfl-fm'});
    }
  }
  if(['create','install'].includes(command)) {
    applications.push(...kernelCredits());
    applications.push(...compiler);
    applications.push(...commanderFiles(), ...riftFiles(), ...unicodeFiles());
    applications.push({name:'/mnt/host',directory:true,mode:0o40755});
    if(appDirectory && fs.existsSync(path.join(appDirectory,'nano-X.fxe')))
      applications.push({name:'/etc/nxlaunch.cnf',mode:0o100644,preserve:true,
        data:Buffer.from('Terminal - /bin/nxterm\nMac-term - /bin/gterm\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nDraw - /bin/nxdraw\nCalculator - /bin/nxcalc\nTetris - /bin/nxtetris\nImages - /bin/nxview\nKlotski - /bin/klotski\n')});
    applications.push({name:'/dev/tty',mode:0o20666,device:0x200,data:Buffer.alloc(0)});
    applications.push({name:'/dev/gfx',mode:0o20666,device:0x400,data:Buffer.alloc(0)});
    applications.push(...ptyDevices());
    applications.push({name:'/dev/urandom',mode:0o20444,device:0x104,data:Buffer.alloc(0)});
    applications.push({name:'/dev/crypto',mode:0o20666,device:0x105,data:Buffer.alloc(0)});
    applications.push({name:'/dev/pcm',mode:0o20666,device:0x500,data:Buffer.alloc(0)});
    for(const [name,device] of [['null',0x100],['zero',0x102],['proc',0x103]])
      applications.push({name:`/dev/${name}`,mode:0o20666,device,data:Buffer.alloc(0)});
    for(const [name,data] of [
      ['/etc/passwd','root:x:0:0:root:/root:/bin/sh\n'],['/etc/group','root:x:0:root\n'],
      ['/etc/mtab','/dev/root / riplix rw 0 0\n'],
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
    applications.push(...rootHomeFiles());
  }
  if (rest.length || !source || !['create','install','put','extract'].includes(command) ||
      (command==='put' ? (!destination || !extra || !destination.startsWith('/')) :
       command === 'extract' ? (!destination || extra) : (destination !== undefined))) {
    throw Error('usage: image.mjs create ROOT [--apps DIR] | install ROOT [--apps DIR] | put ROOT /guest/path HOSTFILE | extract RIPPLE-DISK ROOT');
  }
  if(command==='put') applications.splice(0,applications.length,{name:destination,mode:0o100755,data:fs.readFileSync(extra)});
  if (command === 'install' || command==='put') {
    const original = openImage(source), updated = installRoot(
      growRoot(original, ROOT_BLOCKS, compiler.length ? 2048 : 1024), applications);
    if (!original.equals(updated)) {
      const backup = backupImage(source, original);
      atomicWrite(source, updated);
      pruneImageBackups(source);
      console.log(backup ? `Installed native programs; previous image: ${backup}` : 'Installed native programs (backups disabled)');
    }
    process.exit(0);
  }
  const output = command === 'create' ? source : destination;
  const image = command === 'create'
    ? rootImage(applications, ROOT_BLOCKS, compiler.length ? 2048 : 1024)
    : growRoot(fatFile(openImage(source), 'ROOTFS.IMG').data, ROOT_BLOCKS);
  writeImage(output, image, true);
  console.log(`Created ${output}`);
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
