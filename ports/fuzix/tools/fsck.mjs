// Offline checker for the Ripple C16 FUZIX filesystem. Use fsck.py for locking.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

import {atomicWrite} from './disk.mjs';
import {fatFile} from './fat.mjs';
import {backupImage, pruneImageBackups} from './image-backup.mjs';
import {BLOCK, CELLS, layout, baseFor} from './layout.mjs';
import {Image, openImage, writeImage} from './image.mjs';

export function checkRoot(input, repair = false) {
  const image = Image.from(input).fork(), w = p => image.readUInt16LE(p * 2),
        put = (p, v) => image.writeUInt16LE(v, p * 2), wide = p => image.readUInt32LE(p * 2);
  const format = layout(image), {sb: offsets, ptr, step, direct, indirect: entries} = format;
  const setptr = (p, v) => format.version === 3 ? image.writeUInt32LE(v, p * 2) : put(p, v);
  const sb = CELLS, isize = w(sb + offsets.isize), blocks = ptr(sb + offsets.blocks),
        inodes = (isize - 2) * format.inodes, issues = [],
        owners = new Map(), nodes = new Map(), refs = new Map();
  if (isize < 3 || blocks <= isize || image.length !== blocks * BLOCK || inodes > 65535)
    throw Error('invalid root geometry');
  function claim(b, ino) {
    if (b < isize || b >= blocks) throw Error(`inode ${ino}: block outside data area`);
    if (owners.has(b))
      throw Error(`block ${b} is multiply allocated by inodes ${owners.get(b)} and ${ino}`);
    owners.set(b, ino);
  }
  function indirect(b, level, start, map, ino) {
    if (!b) return;
    claim(b, ino);
    for (let j = 0; j < entries; j++) {
      const v = ptr(b * CELLS + j * step);
      if (!v) continue;
      if (level === 1) {
        claim(v, ino);
        map.set(start + j, v);
      } else
        indirect(v, level - 1, start + j * entries ** (level - 1), map, ino);
    }
  }
  for (let ino = 1; ino < inodes; ino++) {
    const base = baseFor(ino, format), mode = w(base), links = w(base + 1),
          map = new Map(), size = wide(base + 4);
    if (!mode && !links) continue;
    const type = mode & 0xf000;
    if (![0x8000, 0x4000, 0x2000, 0x6000, 0x1000, 0xc000, 0xa000].includes(type))
      throw Error(`inode ${ino}: invalid type`);
    // Pathname sockets have no stored stream state or filesystem data blocks.
    if (type === 0xc000 && (size || Array.from({length: 20}, (_, j) =>
        ptr(base + 12 + j * step)).some(Boolean)))
      throw Error(`inode ${ino}: invalid socket metadata`);
    // FUZIX stores named-pipe buffers in ordinary filesystem blocks too.
    if (type === 0x8000 || type === 0x4000 || type === 0x1000 || type === 0xa000) {
      for (let j = 0; j < direct; j++) {
        const b = ptr(base + 12 + j * step);
        if (b) {
          claim(b, ino);
          map.set(j, b);
        }
      }
      let start = direct;
      for (let level = 1; level <= (format.version === 3 ? 3 : 2); level++) {
        indirect(ptr(base + 12 + (direct + level - 1) * step), level, start, map, ino);
        start += entries ** level;
      }
    }
    if (size > 0x7fffffff) throw Error(`inode ${ino}: invalid file size`);
    nodes.set(ino, {ino, base, type, links, map, size});
  }
  const root = nodes.get(1);
  if (!root || root.type !== 0x4000) throw Error('root directory missing');
  function cell(node, pos) {
    const b = node.map.get(Math.floor(pos / CELLS));
    return b ? w(b * CELLS + pos % CELLS) : 0;
  }
  // Link targets are text, not inode references. Dangling links and cycles
  // are legal; only their storage is checked here.
  for (const node of nodes.values()) {
    if (node.type !== 0xa000) continue;
    if (!node.size || node.size >= 512 || node.map.size !== 1 || !node.map.has(0))
      throw Error(`inode ${node.ino}: invalid symbolic link size or blocks`);
    for (let pos = 0; pos < node.size; pos++)
      if (!cell(node, pos)) throw Error(`inode ${node.ino}: NUL in symbolic link target`);
  }
  const reachable = new Set(), parents = new Map([[1, 1]]), queue = [1];
  while (queue.length) {
    const ino = queue.shift();
    if (reachable.has(ino)) continue;
    reachable.add(ino);
    const node = nodes.get(ino);
    if (node.type !== 0x4000) continue;
    if (node.size % 32 || node.size > blocks * CELLS) throw Error(`directory ${ino}: invalid size`);
    let dot = false, dotdot = false;
    for (let off = 0; off < node.size; off += 32) {
      const child = cell(node, off);
      if (!child) continue;
      const target = nodes.get(child);
      if (!target) throw Error(`directory ${ino}: missing inode ${child}`);
      let name = '';
      for (let j = 1; j <= 30 && cell(node, off + j); j++)
        name += String.fromCharCode(cell(node, off + j));
      refs.set(child, (refs.get(child) || 0) + 1);
      if (name === '.') {
        if (child !== ino) throw Error('invalid . entry');
        dot = true;
      } else if (name === '..') {
        if (child !== parents.get(ino)) throw Error('invalid .. entry');
        dotdot = true;
      } else {
        if (!name || name.includes('/')) throw Error('invalid directory name');
        if (target.type === 0x4000) {
          if (parents.has(child)) throw Error('directory cycle or multiple parents');
          parents.set(child, ino);
        }
        queue.push(child);
      }
    }
    if (!dot || !dotdot) throw Error(`directory ${ino}: missing dot entries`);
  }
  const abandonedPipes = [];
  for (const [ino, node] of nodes) {
    if (!reachable.has(ino)) {
      // pipe() allocates a disk inode with no directory link. An interrupted
      // session, or shutdown with a pipe still open, can leave it allocated.
      // Only this disposable IPC state is recoverable without a pathname;
      // orphaned regular files/directories still require manual recovery.
      if (node.type === 0x1000 && node.links === 0) {
        abandonedPipes.push(node);
        continue;
      }
      throw Error(`unreachable inode ${ino}; refusing automatic repair`);
    }
    if (node.links !== (refs.get(ino) || 0))
      throw Error(`inode ${ino}: link count mismatch; refusing automatic repair`);
  }
  // Reachability, all directory links and every block owner have now passed
  // validation. Reclaim in the private image only, before rebuilding free lists.
  for (const node of abandonedPipes) {
    issues.push(`unlinked pipe inode ${node.ino} left allocated`);
    if (!repair) continue;
    for (const [block, owner] of owners)
      if (owner === node.ino) owners.delete(block);
    image.fill(0, node.base * 2, (node.base + format.cells) * 2);
    nodes.delete(node.ino);
  }
  const freeExpected = [];
  for (let b = isize; b < blocks; b++)
    if (!owners.has(b)) freeExpected.push(b);
  const unused = [];
  for (let i = 2; i < inodes; i++)
    if (!nodes.has(i)) unused.push(i);
  try {
    const seen = new Set();
    let n = w(sb + offsets.nfree), list = Array.from({length: 50}, (_, i) => ptr(sb + offsets.free + i * step));
    for (;;) {
      if (n < 1 || n > 50) throw Error('invalid free block count');
      const b = list[--n];
      if (!b) {
        if (n) throw Error('early free-list terminator');
        break;
      }
      if (b < isize || b >= blocks || owners.has(b) || seen.has(b))
        throw Error('invalid or duplicate free block');
      seen.add(b);
      if (!n) {
        n = w(b * CELLS);
        list = Array.from({length: 50}, (_, i) => ptr(b * CELLS + 1 + i * step));
      }
    }
    if (seen.size !== freeExpected.length || ptr(sb + offsets.tfree) !== seen.size)
      throw Error('free block totals mismatch');
  } catch (e) {
    issues.push(e.message);
  }
  if (w(sb + offsets.tinode) !== unused.length) issues.push('free inode total mismatch');
  const ni = w(sb + offsets.ninode), cached = new Set();
  if (ni > 50)
    issues.push('invalid inode cache size');
  else
    for (let i = 0; i < ni; i++) {
      const ino = w(sb + offsets.inode + i);
      if (!unused.includes(ino) || cached.has(ino)) {
        issues.push('invalid free inode cache');
        break;
      }
      cached.add(ino);
    }
  if (w(sb + offsets.clean) !== 2) issues.push('filesystem was not shut down cleanly');
  if (repair && issues.length) {
    let free = [0];
    for (const b of [...freeExpected].reverse()) {
      if (free.length === 50) {
        put(b * CELLS, 50);
        free.forEach((v, i) => setptr(b * CELLS + 1 + i * step, v));
        free = [];
      }
      free.push(b);
    }
    put(sb + offsets.nfree, free.length);
    for (let i = 0; i < 50; i++) setptr(sb + offsets.free + i * step, free[i] || 0);
    setptr(sb + offsets.tfree, freeExpected.length);
    put(sb + offsets.ninode, Math.min(50, unused.length));
    for (let i = 0; i < 50; i++) put(sb + offsets.inode + i, unused[i] || 0);
    put(sb + offsets.tinode, unused.length);
    put(sb + offsets.clean, 2);
  }
  return {owners, image, issues, files: nodes.size, freeBlocks: freeExpected.length, nodes, format, isize, blocks};
}
export function checkDisk(filename, repair = false) {
  const original = openImage(filename), root = fatFile(original, 'ROOTFS.IMG'),
        result = checkRoot(root.data, repair);
  for (const message of result.issues) console.error(message);
  if (repair && result.issues.length) {
    if (checkRoot(result.image).issues.length) throw Error('repaired root failed validation');
    backupImage(filename, original);
    atomicWrite(filename, root.replace(result.image));
    pruneImageBackups(filename);
  }
  console.log(`${result.files} inodes; ${result.freeBlocks} free blocks; ${
      result.issues.length ? (repair ? 'repaired' : 'needs repair') : 'clean'}`);
  return !result.issues.length || repair;
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    if (process.argv[2] !== '--locked') throw Error('use tools/fsck.py so the disk is locked');
    if (!checkDisk(process.argv[3], process.argv[4] === '--repair')) process.exitCode = 1;
  } catch (error) {
    console.error(`Filesystem check failed: ${error.message}`);
    process.exitCode = 1;
  }
}
