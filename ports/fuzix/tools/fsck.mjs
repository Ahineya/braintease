// Offline checker for the Ripple C16 FUZIX filesystem. Use fsck.py for locking.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

import {atomicWrite} from './disk.mjs';
import {fatFile} from './fat.mjs';

export function checkRoot(input, repair = false) {
  const image = Buffer.from(input), w = p => image.readUInt16LE(p * 2),
        put = (p, v) => image.writeUInt16LE(v & 65535, p * 2), wide = p => w(p) + w(p + 1) * 65536;
  if (image.length < 1024 || image.toString('ascii', 0, 19) !== 'RIPPLE-FUZIX-C16-V1' ||
      w(256) !== 0x7252)
    throw Error('not a Ripple C16 FUZIX root');
  const sb = 256, isize = w(sb + 1), blocks = w(sb + 2), inodes = (isize - 2) * 8, issues = [],
        owners = new Map(), nodes = new Map(), refs = new Map();
  if (isize < 3 || blocks <= isize || image.length !== blocks * 512)
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
    for (let j = 0; j < 256; j++) {
      const v = w(b * 256 + j);
      if (!v) continue;
      if (level === 1) {
        claim(v, ino);
        map.set(start + j, v);
      } else
        indirect(v, level - 1, start + j * 256, map, ino);
    }
  }
  for (let ino = 1; ino < inodes; ino++) {
    const base = (2 + (ino >> 3)) * 256 + (ino & 7) * 32, mode = w(base), links = w(base + 1),
          map = new Map(), size = wide(base + 4);
    if (!mode && !links) continue;
    const type = mode & 0xf000;
    if (![0x8000, 0x4000, 0x2000, 0x6000, 0x1000].includes(type))
      throw Error(`inode ${ino}: invalid type`);
    // FUZIX stores named-pipe buffers in ordinary filesystem blocks too.
    if (type === 0x8000 || type === 0x4000 || type === 0x1000) {
      for (let j = 0; j < 18; j++) {
        const b = w(base + 12 + j);
        if (b) {
          claim(b, ino);
          map.set(j, b);
        }
      }
      indirect(w(base + 30), 1, 18, map, ino);
      indirect(w(base + 31), 2, 274, map, ino);
    }
    nodes.set(ino, {ino, base, type, links, map, size});
  }
  const root = nodes.get(1);
  if (!root || root.type !== 0x4000) throw Error('root directory missing');
  function cell(node, pos) {
    const b = node.map.get(Math.floor(pos / 256));
    return b ? w(b * 256 + pos % 256) : 0;
  }
  const reachable = new Set(), parents = new Map([[1, 1]]), queue = [1];
  while (queue.length) {
    const ino = queue.shift();
    if (reachable.has(ino)) continue;
    reachable.add(ino);
    const node = nodes.get(ino);
    if (node.type !== 0x4000) continue;
    if (node.size % 32 || node.size > blocks * 256) throw Error(`directory ${ino}: invalid size`);
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
  for (const [ino, node] of nodes) {
    if (!reachable.has(ino)) throw Error(`unreachable inode ${ino}; refusing automatic repair`);
    if (node.links !== (refs.get(ino) || 0))
      throw Error(`inode ${ino}: link count mismatch; refusing automatic repair`);
  }
  const freeExpected = [];
  for (let b = isize; b < blocks; b++)
    if (!owners.has(b)) freeExpected.push(b);
  const unused = [];
  for (let i = 2; i < inodes; i++)
    if (!nodes.has(i)) unused.push(i);
  try {
    const seen = new Set();
    let n = w(sb + 3), list = Array.from({length: 50}, (_, i) => w(sb + 4 + i));
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
        n = w(b * 256);
        list = Array.from({length: 50}, (_, i) => w(b * 256 + 1 + i));
      }
    }
    if (seen.size !== freeExpected.length || w(sb + 109) !== seen.size)
      throw Error('free block totals mismatch');
  } catch (e) {
    issues.push(e.message);
  }
  if (w(sb + 110) !== unused.length) issues.push('free inode total mismatch');
  const ni = w(sb + 54), cached = new Set();
  if (ni > 50)
    issues.push('invalid inode cache size');
  else
    for (let i = 0; i < ni; i++) {
      const ino = w(sb + 55 + i);
      if (!unused.includes(ino) || cached.has(ino)) {
        issues.push('invalid free inode cache');
        break;
      }
      cached.add(ino);
    }
  if (w(sb + 105) !== 2) issues.push('filesystem was not shut down cleanly');
  if (repair && issues.length) {
    let free = [0];
    for (const b of [...freeExpected].reverse()) {
      if (free.length === 50) {
        put(b * 256, 50);
        free.forEach((v, i) => put(b * 256 + 1 + i, v));
        free = [];
      }
      free.push(b);
    }
    put(sb + 3, free.length);
    for (let i = 0; i < 50; i++) put(sb + 4 + i, free[i] || 0);
    put(sb + 109, freeExpected.length);
    put(sb + 54, Math.min(50, unused.length));
    for (let i = 0; i < 50; i++) put(sb + 55 + i, unused[i] || 0);
    put(sb + 110, unused.length);
    put(sb + 105, 2);
  }
  return {image, issues, files: nodes.size, freeBlocks: freeExpected.length};
}
export function checkDisk(filename, repair = false) {
  const original = fs.readFileSync(filename), root = fatFile(original, 'ROOTFS.IMG'),
        result = checkRoot(root.data, repair);
  for (const message of result.issues) console.error(message);
  if (repair && result.issues.length) {
    let backup = `${filename}.bak`, n = 1;
    while (fs.existsSync(backup)) backup = `${filename}.bak.${n++}`;
    fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
    atomicWrite(filename, root.replace(result.image));
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
