#!/usr/bin/env node
// Ripple FUZIX uses 256 16-bit C characters per physical 512-octet block.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {fatFile} from './fat.mjs';
import {fat16Image, updateFat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';
const BLOCK = 512, CELLS = 256, MAGIC = 0x7252;
export function rootImage(files, blocks = 8192) {
  if (blocks < 128 || blocks > 65535) throw Error('invalid root size');
  const image = Buffer.alloc(blocks * BLOCK), isize = 18;
  let nextBlock = isize, nextInode = 1;
  const nodes = [];
  const word = (cell, v) => image.writeUInt16LE(v & 65535, cell * 2);
  const wide = (cell, v) => {
    word(cell, v);
    word(cell + 1, v >>> 16);
  };
  function alloc() {
    if (nextBlock >= blocks) throw Error('root image full');
    return nextBlock++;
  }
  function inode(mode, data, links = 1, device = 0) {
    const ino = nextInode++;
    if (ino >= 128) throw Error('too many seed files');
    const base = (2 + (ino >> 3)) * CELLS + (ino & 7) * 32;
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
    for (let i = 0; i < Math.min(18, list.length); i++) word(base + 12 + i, list[i]);
    if (list.length > 18) {
      const b = alloc();
      word(base + 30, b);
      for (let i = 18; i < Math.min(274, list.length); i++) word(b * CELLS + i - 18, list[i]);
    }
    if (list.length > 274) {
      const root = alloc();
      word(base + 31, root);
      for (let i = 274; i < list.length; i += 256) {
        const b = alloc();
        word(root * CELLS + Math.floor((i - 274) / 256), b);
        for (let j = i; j < Math.min(i + 256, list.length); j++) word(b * CELLS + j - i, list[j]);
      }
    }
    return node;
  }
  // Allocate directories after numbering nodes: directory contents need inode IDs.
  function directory(entries) {
    const out = [];
    for (const [name, ino] of entries) {
      if (name.length > 30) throw Error('name too long');
      out.push(ino, ...Array.from({length: 30}, (_, i) => name.charCodeAt(i) || 0), 0);
    }
    return out;
  }
  // Root inode #1, /bin #2, /dev #3. Fill their data later.
  const root = inode(0o40755, [], 4), bin = inode(0o40755, [], 2), dev = inode(0o40755, [], 2);
  const tty = inode(0o20666, [], 1, 0x201);
  const entries = [['.', 1], ['..', 1], ['bin', 2], ['dev', 3]], binaries = [['.', 2], ['..', 1]];
  for (const f of files) {
    const node = inode(f.mode ?? 0o100755, Array.from(f.data));
    if (f.name === '/init')
      entries.push(['init', node.ino]);
    else
      binaries.push([path.basename(f.name), node.ino]);
  }
  const text =
      'Welcome to FUZIX on Ripple.\nFiles created here survive normal launches and kernel rebuilds.\nUse shutdown to flush the filesystem before stopping.\n';
  const readme = inode(0o100644, Array.from(text, c => c.charCodeAt(0)));
  entries.push(['README', readme.ino]);
  for (const [node, items] of [
           [root, entries], [bin, binaries], [dev, [['.', 3], ['..', 1], ['tty1', tty.ino]]]]) {
    const data = directory(items), block = alloc();
    if (data.length > CELLS) throw Error('seed directory too large');
    wide(node.base + 4, data.length);
    word(node.base + 12, block);
    data.forEach((v, i) => word(block * CELLS + i, v));
  }
  // Inode zero is reserved so the kernel's free-inode scan cannot allocate it.
  word(2 * CELLS, 0o100000);
  word(2 * CELLS + 1, 1);
  let free = [0], tfree = 0;
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
  const unused = 128 - nextInode;
  word(base + 54, Math.min(50, unused));
  for (let i = 0; i < Math.min(50, unused); i++) word(base + 55 + i, nextInode + i);
  word(base + 105, 2);
  wide(base + 107, 1792108800);
  word(base + 109, tfree);
  word(base + 110, unused);
  word(base + 111, 0);
  // Signature distinguishes this ABI from conventional eight-bit FUZIX images.
  image.write('RIPPLE-FUZIX-C16-V1', 0, 'ascii');
  return image;
}
export function atomicWrite(filename, data) {
  fs.mkdirSync(path.dirname(filename), {recursive: true});
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
    if (fd !== undefined) fs.closeSync(fd);
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
}
export function buildDisk(build, filename, fresh = false) {
  const kernel = {name: 'FUZIX.SYS', data: fs.readFileSync(path.join(build, 'FUZIX.SYS'))};
  let disk;
  if (fs.existsSync(filename) && !fresh) {
    const old = fs.readFileSync(filename);
    const root = fatFile(old, 'ROOTFS.IMG').data;
    if (root.toString('ascii', 0, 19) !== 'RIPPLE-FUZIX-C16-V1')
      throw Error('existing disk has incompatible root ABI; use a different disk or --fresh');
    disk = updateFat16Image(old, [kernel]);
    if (disk.equals(old)) return false;
    let backup = `${filename}.bak`, i = 1;
    while (fs.existsSync(backup)) backup = `${filename}.bak.${i++}`;
    fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
  } else {
    const files = ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'].map(
        name => ({
          name: name === 'init' ? '/init' : `/bin/${name}`,
          data: fs.readFileSync(path.join(build, `${name}.rxe`))
        }));
    disk = fat16Image([kernel, {name: 'ROOTFS.IMG', data: rootImage(files)}]);
    if (fs.existsSync(filename)) {
      let backup = `${filename}.bak`, i = 1;
      while (fs.existsSync(backup)) backup = `${filename}.bak.${i++}`;
      fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
    }
  }
  atomicWrite(filename, disk);
  return true;
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [build, filename, flag] = process.argv.slice(2);
  if (!build || !filename || flag && flag !== '--fresh')
    throw Error('usage: disk.mjs BUILD IMAGE [--fresh]');
  console.error(
      buildDisk(build, filename, flag === '--fresh') ? `Updated ${filename}` :
                                                       `Disk already current: ${filename}`);
}
