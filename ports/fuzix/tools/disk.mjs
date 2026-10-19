#!/usr/bin/env node
import {ptyDevices} from './pty.mjs';
// Ripple FUZIX C16 V3 uses 2,048 physical octets per filesystem block.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {fatFile} from './fat.mjs';
import {installRoot, growRoot} from './install.mjs';
import {manualFiles} from './manuals.mjs';
import {sokobanFiles} from './sokoban.mjs';
import {commanderFiles, riftFiles} from '../../../rfl/os/files.mjs';
import {basicFiles} from '../../basic/files.mjs';
import {lynxFiles} from '../../lynx/files.mjs';
import {tlsFiles} from '../../tls/files.mjs';
import {fuseDEFiles} from '../../fusede/files.mjs';
import {compilerFiles} from '../../../rcc-portable/files.mjs';
import {unicodeFiles} from '../../../libraries/unicode/files.mjs';
import {netsurfFiles} from '../../netsurf/files.mjs';
import {
  fat16Image,
  updateFat16Image
} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {BLOCK, CELLS, INODES_PER_BLOCK, MAGIC, SIGNATURE, SB, ROOT_BLOCKS, inodeBase, DISK_GEOMETRY, layout} from './layout.mjs';
import {Image, openImage, writeImage} from './image.mjs';
import {checkRoot} from './fsck.mjs';
import {backupImage, pruneImageBackups} from './image-backup.mjs';
export function rootImage(files, blocks = ROOT_BLOCKS, inodeCount = 1024) {
  if (!Number.isInteger(inodeCount) || inodeCount < 16 || inodeCount % INODES_PER_BLOCK || inodeCount > 65520)
    throw Error('invalid inode count');
  const isize = 2 + inodeCount / INODES_PER_BLOCK;
  if (!Number.isInteger(blocks) || blocks <= isize + 8 || blocks * BLOCK > 0xffffffff) throw Error('invalid root size');
  const image = new Image(blocks * BLOCK), put = (p, v) => image.writeUInt16LE(v, p * 2);
  image.write(SIGNATURE, 0, 'ascii'); put(CELLS, MAGIC); put(CELLS + SB.isize, isize);
  image.writeUInt32LE(blocks, (CELLS + SB.blocks) * 2);
  image.writeUInt32LE(1792108800, (CELLS + SB.time) * 2);
  put(inodeBase(0), 0o100000); put(inodeBase(0) + 1, 1);
  put(inodeBase(1), 0o40755); put(inodeBase(1) + 1, 2);
  image.writeUInt32LE(64, (inodeBase(1) + 4) * 2);
  image.writeUInt32LE(isize, (inodeBase(1) + 12) * 2);
  for (const [at, name] of [[isize * CELLS, '.'], [isize * CELLS + 32, '..']]) {
    put(at, 1); for (let j = 0; j < name.length; j++) put(at + 1 + j, 46);
  }
  const clean = checkRoot(image, true).image;
  return installRoot(clean, [
    ...['/bin', '/dev', '/etc', '/tmp'].map(name => ({name, directory: true, mode: name === '/tmp' ? 0o41777 : 0o40755})),
    {name: '/dev/tty1', mode: 0o20666, device: 0x201},
    ...files,
    {name: '/README', mode: 0o100644, data: Buffer.from('Welcome to FUZIX on Ripple.\nFiles created here survive normal launches and kernel rebuilds.\nUse shutdown to flush the filesystem before stopping.\n')}
  ]);
}
export function atomicWrite(filename, data) {
  fs.mkdirSync(path.dirname(filename), {recursive : true});
  const temporary = `${filename}.tmp-${process.pid}`;
  try {
    writeImage(temporary, data, true);
    fs.renameSync(temporary, filename);
    const dir = fs.openSync(path.dirname(filename), 'r');
    try {
      fs.fsyncSync(dir);
    } finally {
      fs.closeSync(dir);
    }
  } finally {
    if (fs.existsSync(temporary))
      fs.unlinkSync(temporary);
  }
}
export function seedNames(build) {
  const manifest = path.join(build, 'apps.json');
  return fs.existsSync(manifest)
             ? JSON.parse(fs.readFileSync(manifest, 'utf8'))
             : [ 'init', 'hello', 'forkcheck', 'preemptcheck', 'iotest' ];
}
export function buildDisk(build, filename, fresh = false, install = false) {
  const compiler = compilerFiles(path.join(build, 'compiler'));
  const aliases = {vi: '/bin/rift', mc: '/bin/rfl-fm', mv: '/bin/cp', ln: '/bin/cp',
                   tcl: '/bin/picol', tclsh: '/bin/picol', sokoban: '/bin/sok', nslookup: '/bin/host'};
  // Switch aliases immediately after their target. This releases an older
  // shared executable before allocating the next large program during upgrades.
  const installedNames = seedNames(build), orderedNames = [];
  for (const name of installedNames)
    if (aliases[name] && !installedNames.includes(path.basename(aliases[name])))
      throw Error(`missing executable alias target: ${aliases[name]}`);
  for (const name of installedNames) {
    if (aliases[name]) continue;
    orderedNames.push(name);
    for (const [alias, target] of Object.entries(aliases))
      if (target === `/bin/${name}` && installedNames.includes(alias))
        orderedNames.push(alias);
  }
  const seedFiles = () =>
      [...orderedNames.map(
           name => ({
             name : name === 'init' ? '/init' : `/bin/${name}`,
             ...(aliases[name] ? {symlink: path.basename(aliases[name])} :
                  {data : fs.readFileSync(path.join(build, `${name}.rxe`))})
           })),
       {
         name : '/tmp',
         directory : true,
         mode : 0o41777,
         data : Buffer.alloc(0)
       },
       // Before job control, /dev/tty addressed the fixed console directly.
       {name : '/dev/tty', mode : 0o20666, device : 0x200,
        migrateDeviceFrom : 0x201, data : Buffer.alloc(0)},
       ...ptyDevices(),
       {name : '/dev/gfx', mode : 0o20600, device : 0x400, data : Buffer.alloc(0)},
       {name : '/dev/root', mode : 0o60600, device : 0, data : Buffer.alloc(0)},
       {name:'/dev/urandom',mode:0o20444,device:0x104,data:Buffer.alloc(0)},
       {name:'/dev/crypto',mode:0o20666,device:0x105,data:Buffer.alloc(0)},
       {name:'/dev/pcm',mode:0o20666,device:0x500,data:Buffer.alloc(0)},
       ...['null', 'zero', 'proc'].map((name, i) => ({
                                         name : `/dev/${name}`,
                                         mode : 0o20666,
                                         device : 0x100 + [ 0, 2, 3 ][i],
                                         data : Buffer.alloc(0)
                                       })),
       {
         name : '/etc/passwd',
         mode : 0o100644,
         preserve : true,
         data : Buffer.from('root:x:0:0:root:/:/bin/sh\n')
       },
       {
         name : '/etc/group',
         mode : 0o100644,
         preserve : true,
         data : Buffer.from('root:x:0:root\n')
       },
       {
         name : '/etc/mtab',
         mode : 0o100644,
         preserve : true,
         data : Buffer.from('/dev/root / fuzix rw 0 0\n')
       },
       {name: '/etc/resolv.conf', mode: 0o100644, preserve: true,
        data: Buffer.from('nameserver 1.1.1.1\nnameserver 8.8.8.8\n')},
       {name: '/etc/nxlaunch.cnf', mode: 0o100644, preserve: true,
        data: Buffer.from('Terminal - /bin/nxterm\nEditor - /bin/nxterm /bin/vi\nProcesses - /bin/nxterm /bin/top\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nRoaches - /bin/nxroach\nTetris - /bin/nxtetris\nImages - /bin/nxview\n')},
       {name: '/etc/hosts', mode: 0o100644, preserve: true,
        data: Buffer.from('192.168.64.250 ripple fuzix\n')},
       {
         name: '/etc/termcap', mode: 0o100644, preserve: true,
         data: Buffer.from('ansi|vt100|xterm|xterm-256color|ripple:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')
       },
       {
         name: '/hello.tcl', mode: 0o100644, preserve: true,
         data: fs.readFileSync(fileURLToPath(new URL('../examples/hello.tcl', import.meta.url)))
       },
       ...['makeactions', 'makemacros'].map(name => ({
         name: `/usr/lib/${name}`, mode: 0o100644, preserve: true,
         data: fs.readFileSync(fileURLToPath(new URL(`../upstream/Applications/MWC/cmd/${name}`, import.meta.url)))
       })),
       ...commanderFiles(), ...riftFiles(), ...unicodeFiles(),
       ...(installedNames.includes('basic') ? basicFiles() : []),
       ...(installedNames.includes('lynx') ? lynxFiles() : []),
       ...(installedNames.includes('netsurf') ? netsurfFiles() : []),
       ...((installedNames.includes('htget') || installedNames.includes('lynx') || installedNames.includes('chat')) ? tlsFiles() : []),
       ...(installedNames.includes('fdeshowcase') ? fuseDEFiles(path.join(build,'fusede')) : []),
       ...sokobanFiles(),
       ...manualFiles(), ...compiler];
  const kernel = {
    name : 'FUZIX.SYS',
    data : fs.readFileSync(path.join(build, 'FUZIX.SYS'))
  };
  let disk;
  if (fs.existsSync(filename) && !fresh) {
    const old = openImage(filename);
    const root = fatFile(old, 'ROOTFS.IMG').data;
    const format = layout(root);
    if (format.version !== 3 && !install) throw Error('C16 V2 disk needs an install/upgrade before booting the V3 kernel');
    if (install) {
      const updated = installRoot(growRoot(root, ROOT_BLOCKS, compiler.length ? 2048 : 1024), seedFiles());
      if (updated.length !== root.length || format.version !== 3) {
        // The original bootstrap disk was too small for Unix userspace. Keep
        // inode identities and file contents while expanding the native root.
        // Refuse unexpected outer layouts rather than dropping their entries.
        const bps = old.readUInt16LE(11),
              start =
                  (old.readUInt16LE(14) + old.readUInt8(16) * old.readUInt16LE(22)) * bps;
        for (let i = 0; i < old.readUInt16LE(17); i++) {
          const at = start + i * 32;
          if (!old.readUInt8(at))
            break;
          if (old.readUInt8(at) === 0xe5 || old.readUInt8(at + 11) & 8)
            continue;
          const name = old.toString('ascii', at, at + 11);
          if (!['FUZIX   SYS', 'ROOTFS  IMG'].includes(name))
            throw Error(
                'cannot expand a disk with additional outer FAT entries; use a new default image');
        }
        disk = fat16Image([ kernel, {name : 'ROOTFS.IMG', data : updated} ],
                          {...DISK_GEOMETRY, imageFactory: size => new Image(size)});
      } else
        disk = updateFat16Image(fatFile(old, 'ROOTFS.IMG').replace(updated),
                                [ kernel ]);
    } else
      disk = updateFat16Image(old, [ kernel ]);
    if (disk.equals(old))
      return false;
    backupImage(filename, old);
  } else {
    const files = seedFiles();
    disk =
        fat16Image([ kernel, {name : 'ROOTFS.IMG', data : rootImage(files, ROOT_BLOCKS, compiler.length ? 2048 : 1024)} ],
                   {...DISK_GEOMETRY, imageFactory: size => new Image(size)});
    if (fs.existsSync(filename)) {
      backupImage(filename, openImage(filename));
    }
  }
  atomicWrite(filename, disk);
  pruneImageBackups(filename);
  return true;
}
if (process.argv[1] &&
    path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [build, filename, ...flags] = process.argv.slice(2);
    if (!build || !filename ||
        flags.some(f => !['--fresh', '--install'].includes(f)))
      throw Error('usage: disk.mjs BUILD IMAGE [--fresh] [--install]');
    console.error(buildDisk(build, filename, flags.includes('--fresh'),
                            flags.includes('--install'))
                      ? `Updated ${filename}`
                      : `Disk already current: ${filename}`);
  } catch (error) {
    console.error(`Disk update failed: ${error.message}`);
    process.exitCode = 1;
  }
}
