#!/usr/bin/env node
// Ripple FUZIX C16 V2 uses 2,048 physical octets per filesystem block.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {fatFile} from './fat.mjs';
import {installRoot, growRoot} from './install.mjs';
import {manualFiles} from './manuals.mjs';
import {sokobanFiles} from './sokoban.mjs';
import {
  fat16Image,
  updateFat16Image
} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {BLOCK, CELLS, INODES_PER_BLOCK, MAGIC, SIGNATURE, inodeBase, DISK_GEOMETRY} from './layout.mjs';
export function rootImage(files, blocks = 65535, inodeCount = 1024) {
  // The bootstrap builder fills its five fixed directories directly. Let the
  // normal installer create deeper trees instead of flattening them into /bin.
  const fixed = ['/', '/bin', '/dev', '/etc', '/tmp'];
  const nested = files.filter(f => f.directory ? !fixed.includes(f.name) :
                                    !fixed.slice(0, 4).includes(path.dirname(f.name)));
  files = files.filter(f => !nested.includes(f));
  if (inodeCount < 16 || inodeCount % INODES_PER_BLOCK || inodeCount > 4096)
    throw Error('invalid inode count');
  if (blocks <= 2 + inodeCount / INODES_PER_BLOCK || blocks > 65535)
    throw Error('invalid root size');
  const image = Buffer.alloc(blocks * BLOCK), isize = 2 + inodeCount / INODES_PER_BLOCK;
  let nextBlock = isize, nextInode = 1;
  const nodes = [];
  const word = (cell, v) => image.writeUInt16LE(v & 65535, cell * 2);
  const wide = (cell, v) => {
    word(cell, v);
    word(cell + 1, v >>> 16);
  };
  function alloc() {
    if (nextBlock >= blocks)
      throw Error('root image full');
    return nextBlock++;
  }
  function inode(mode, data, links = 1, device = 0) {
    const ino = nextInode++;
    if (ino >= inodeCount)
      throw Error('too many seed files');
    const base = inodeBase(ino);
    word(base, mode);
    word(base + 1, links);
    wide(base + 4, data.length);
    const node = {ino, base, data, mode};
    nodes.push(node);
    if (mode & 0x2000) {
      word(base + 12, device);
      return node;
    }
    const list = [];
    for (let start = 0; start < data.length; start += CELLS) {
      const b = alloc();
      list.push(b);
      for (let j = 0; j < Math.min(CELLS, data.length - start); j++)
        word(b * CELLS + j, data[start + j]);
    }
    for (let i = 0; i < Math.min(18, list.length); i++)
      word(base + 12 + i, list[i]);
    if (list.length > 18) {
      const b = alloc();
      word(base + 30, b);
      for (let i = 18; i < Math.min((18 + CELLS), list.length); i++)
        word(b * CELLS + i - 18, list[i]);
    }
    if (list.length > (18 + CELLS)) {
      const root = alloc();
      word(base + 31, root);
      for (let i = (18 + CELLS); i < list.length; i += CELLS) {
        const b = alloc();
        word(root * CELLS + Math.floor((i - (18 + CELLS)) / CELLS), b);
        for (let j = i; j < Math.min(i + CELLS, list.length); j++)
          word(b * CELLS + j - i, list[j]);
      }
    }
    return node;
  }
  // Allocate directories after numbering nodes: directory contents need inode
  // IDs.
  function directory(entries) {
    const out = [];
    for (const [name, ino] of entries) {
      if (name.length > 30)
        throw Error('name too long');
      out.push(ino,
               ...Array.from({length : 30}, (_, i) => name.charCodeAt(i) || 0),
               0);
    }
    return out;
  }
  // Root inode #1, /bin #2, /dev #3. Fill their data later.
  const root = inode(0o40755, [], 6), bin = inode(0o40755, [], 2),
        dev = inode(0o40755, [], 2);
  const tty = inode(0o20666, [], 1, 0x201);
  const etc = inode(0o40755, [], 2), tmp = inode(0o41777, [], 2);
  const entries =
            [
              [ '.', 1 ], [ '..', 1 ], [ 'bin', 2 ], [ 'dev', 3 ],
              [ 'etc', etc.ino ], [ 'tmp', tmp.ino ]
            ],
        binaries = [ [ '.', 2 ], [ '..', 1 ] ],
        config = [ [ '.', etc.ino ], [ '..', 1 ] ],
        devices = [ [ '.', 3 ], [ '..', 1 ], [ 'tty1', tty.ino ] ];
  const named = new Map();
  for (const f of files) {
    if (f.directory)
      continue;
    let node;
    if (f.link) {
      node = named.get(f.link);
      if (!node) throw Error(`missing seed link target: ${f.link}`);
      word(node.base + 1, image.readUInt16LE((node.base + 1) * 2) + 1);
    } else node = inode(f.mode ?? 0o100755, Array.from(f.data), 1, f.device ?? 0);
    named.set(f.name, node);
    if (path.dirname(f.name) === '/')
      entries.push([ path.basename(f.name), node.ino ]);
    else if (path.dirname(f.name) === '/dev')
      devices.push([ path.basename(f.name), node.ino ]);
    else if (path.dirname(f.name) === '/etc')
      config.push([ path.basename(f.name), node.ino ]);
    else
      binaries.push([ path.basename(f.name), node.ino ]);
  }
  const text =
      'Welcome to FUZIX on Ripple.\nFiles created here survive normal launches and kernel rebuilds.\nUse shutdown to flush the filesystem before stopping.\n';
  const readme = inode(0o100644, Array.from(text, c => c.charCodeAt(0)));
  entries.push([ 'README', readme.ino ]);
  for (const [node, items] of [
           [ root, entries ], [ bin, binaries ], [ etc, config ],
           [ tmp, [ [ '.', tmp.ino ], [ '..', 1 ] ] ], [ dev, devices ]]) {
    const data = directory(items);
    if (data.length > 18 * CELLS)
      throw Error('seed directory too large');
    wide(node.base + 4, data.length);
    for (let off = 0; off < data.length; off += CELLS) {
      const block = alloc();
      word(node.base + 12 + off / CELLS, block);
      data.slice(off, off + CELLS)
          .forEach((v, i) => word(block * CELLS + i, v));
    }
  }
  // Inode zero is reserved so the kernel's free-inode scan cannot allocate it.
  word(2 * CELLS, 0o100000);
  word(2 * CELLS + 1, 1);
  let free = [ 0 ], tfree = 0;
  for (let b = blocks - 1; b >= nextBlock; b--) {
    if (free.length === 50) {
      word(b * CELLS, 50);
      free.forEach((v, i) => word(b * CELLS + i + 1, v));
      free = [];
    }
    free.push(b);
    tfree++;
  }
  const base = CELLS;
  word(base, MAGIC);
  word(base + 1, isize);
  word(base + 2, blocks);
  word(base + 3, free.length);
  free.forEach((v, i) => word(base + 4 + i, v));
  const unused = inodeCount - nextInode;
  word(base + 54, Math.min(50, unused));
  for (let i = 0; i < Math.min(50, unused); i++)
    word(base + 55 + i, nextInode + i);
  word(base + 105, 2);
  wide(base + 107, 1792108800);
  word(base + 109, tfree);
  word(base + 110, unused);
  word(base + 111, 0);
  // Signature distinguishes this ABI from conventional eight-bit FUZIX images.
  image.write(SIGNATURE, 0, 'ascii');
  return nested.length ? installRoot(image, nested) : image;
}
export function atomicWrite(filename, data) {
  fs.mkdirSync(path.dirname(filename), {recursive : true});
  const temporary = `${filename}.tmp-${process.pid}`;
  let fd;
  try {
    fd = fs.openSync(temporary, 'wx', 0o600);
    fs.writeFileSync(fd, data);
    fs.fsyncSync(fd);
    fs.closeSync(fd);
    fd = undefined;
    fs.renameSync(temporary, filename);
    const dir = fs.openSync(path.dirname(filename), 'r');
    try {
      fs.fsyncSync(dir);
    } finally {
      fs.closeSync(dir);
    }
  } finally {
    if (fd !== undefined)
      fs.closeSync(fd);
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
  const aliases = {vi: '/bin/levee', mv: '/bin/cp', ln: '/bin/cp',
                   tcl: '/bin/picol', tclsh: '/bin/picol', sokoban: '/bin/sok'};
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
             ...(aliases[name] ? {link: aliases[name]} :
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
       ...Array.from({length: 4}, (_, i) => ([
         {name: `/dev/ptyp${i}`, mode: 0o20600, device: 0x300 + i, data: Buffer.alloc(0)},
         {name: `/dev/ttyp${i}`, mode: 0o20600, device: 0x202 + i, data: Buffer.alloc(0)}
       ])).flat(),
       {name : '/dev/root', mode : 0o60600, device : 0, data : Buffer.alloc(0)},
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
       ...sokobanFiles(),
       ...manualFiles()];
  const kernel = {
    name : 'FUZIX.SYS',
    data : fs.readFileSync(path.join(build, 'FUZIX.SYS'))
  };
  let disk;
  if (fs.existsSync(filename) && !fresh) {
    const old = fs.readFileSync(filename);
    const root = fatFile(old, 'ROOTFS.IMG').data;
    if (root.toString('ascii', 0, 19) !== SIGNATURE)
      throw Error(
          'existing disk has incompatible root ABI; use a different disk or --fresh');
    if (install) {
      const updated = installRoot(growRoot(root), seedFiles());
      if (updated.length !== root.length) {
        // The original bootstrap disk was too small for Unix userspace. Keep
        // inode identities and file contents while expanding the native root.
        // Refuse unexpected outer layouts rather than dropping their entries.
        const bps = old.readUInt16LE(11),
              start =
                  (old.readUInt16LE(14) + old[16] * old.readUInt16LE(22)) * bps;
        for (let i = 0; i < old.readUInt16LE(17); i++) {
          const at = start + i * 32;
          if (!old[at])
            break;
          if (old[at] === 0xe5 || old[at + 11] & 8)
            continue;
          const name = old.toString('ascii', at, at + 11);
          if (!['FUZIX   SYS', 'ROOTFS  IMG'].includes(name))
            throw Error(
                'cannot expand a disk with additional outer FAT entries; use a new default image');
        }
        disk = fat16Image([ kernel, {name : 'ROOTFS.IMG', data : updated} ],
                          DISK_GEOMETRY);
      } else
        disk = updateFat16Image(fatFile(old, 'ROOTFS.IMG').replace(updated),
                                [ kernel ]);
    } else
      disk = updateFat16Image(old, [ kernel ]);
    if (disk.equals(old))
      return false;
    let backup = `${filename}.bak`, i = 1;
    while (fs.existsSync(backup))
      backup = `${filename}.bak.${i++}`;
    fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
  } else {
    const files = seedFiles();
    disk =
        fat16Image([ kernel, {name : 'ROOTFS.IMG', data : rootImage(files)} ],
                   DISK_GEOMETRY);
    if (fs.existsSync(filename)) {
      let backup = `${filename}.bak`, i = 1;
      while (fs.existsSync(backup))
        backup = `${filename}.bak.${i++}`;
      fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
    }
  }
  atomicWrite(filename, disk);
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
