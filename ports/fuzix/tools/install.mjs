// Install managed userspace files into a clean C16 root, preserving other
// inodes.
import {checkRoot} from './fsck.mjs';
import {BLOCK, CELLS, INODES_PER_BLOCK, inodeBase} from './layout.mjs';

export function installRoot(input, files) {
  const image = Buffer.from(input), w = p => image.readUInt16LE(p * 2),
        put = (p, v) => image.writeUInt16LE(v & 65535, p * 2),
        wide = p => w(p) + w(p + 1) * 65536, putWide = (p, v) => {
          put(p, v);
          put(p + 1, v >>> 16);
        };
  // Structural errors always abort before any output can be written.
  const checked = checkRoot(input), isize = w((CELLS + 1)), blocks = w((CELLS + 2)),
        limit = (isize - 2) * INODES_PER_BLOCK;
  const nodes = new Map(), used = new Set();
  function readNode(ino) {
    const base = inodeBase(ino), map = [], owned = [];
    function indirect(b, level) {
      if (!b)
        return;
      used.add(b);
      owned.push(b);
      for (let j = 0; j < CELLS; j++) {
        const v = w(b * CELLS + j);
        if (level === 1) {
          map.push(v);
          if (v) {
            used.add(v);
            owned.push(v);
          }
        } else if (v)
          indirect(v, level - 1);
        else
          for (let k = 0; k < CELLS; k++)
            map.push(0);
      }
    }
    if ([0x8000, 0x4000, 0x1000].includes(w(base) & 0xf000)) {
      for (let j = 0; j < 18; j++) {
        const b = w(base + 12 + j);
        map.push(b);
        if (b) {
          used.add(b);
          owned.push(b);
        }
      }
      indirect(w(base + 30), 1);
      // Preserve the logical gap when no single-indirect tree exists.
      while (map.length < (18 + CELLS))
        map.push(0);
      indirect(w(base + 31), 2);
    }
    return {ino, base, map, owned, size : wide(base + 4)};
  }
  for (let ino = 1; ino < limit; ino++) {
    const base = inodeBase(ino);
    if (w(base) || w(base + 1))
      nodes.set(ino, readNode(ino));
  }
  function cells(node) {
    return Array.from(
        {length : node.size},
        (_, i) => node.map[Math.floor(i / CELLS)] ? w(node.map[Math.floor(i / CELLS)] * CELLS + (i % CELLS)) : 0);
  }
  function lookup(dir, name) {
    const data = cells(dir);
    for (let i = 0; i < data.length; i += 32) {
      if (!data[i])
        continue;
      let text = '';
      for (let j = 1; j <= 30 && data[i + j]; j++)
        text += String.fromCharCode(data[i + j]);
      if (text === name)
        return {ino : data[i], offset : i};
    }
    return null;
  }
  let cursor = isize;
  function alloc() {
    while (cursor < blocks && used.has(cursor))
      cursor++;
    if (cursor === blocks)
      throw Error(
          'not enough free root space to install apps; use a larger fresh disk');
    const b = cursor++;
    used.add(b);
    image.fill(0, b * BLOCK, (b + 1) * BLOCK);
    return b;
  }
  function writeNode(node, data) {
    for (const b of node.owned) {
      used.delete(b);
      if (b < cursor)
        cursor = b;
    }
    node.owned = [];
    node.map = [];
    image.fill(0, (node.base + 12) * 2, (node.base + 32) * 2);
    function block() {
      const b = alloc();
      node.owned.push(b);
      return b;
    }
    const count = Math.ceil(data.length / CELLS);
    let single = 0, double = 0, branch = 0;
    for (let i = 0; i < count; i++) {
      const b = block();
      node.map.push(b);
      if (i < 18)
        put(node.base + 12 + i, b);
      else if (i < (18 + CELLS)) {
        if (!single) {
          single = block();
          put(node.base + 30, single);
        }
        put(single * CELLS + i - 18, b);
      } else {
        if (!double) {
          double = block();
          put(node.base + 31, double);
        }
        if ((i - (18 + CELLS)) % CELLS === 0) {
          branch = block();
          put(double * CELLS + Math.floor((i - (18 + CELLS)) / CELLS), branch);
        }
        put(branch * CELLS + (i - (18 + CELLS)) % CELLS, b);
      }
      for (let j = 0; j < Math.min(CELLS, data.length - i * CELLS); j++)
        put(b * CELLS + j, data[i * CELLS + j]);
    }
    node.size = data.length;
    putWide(node.base + 4, data.length);
  }
  let changed = false;
  function newNode(mode, links = 1) {
    if (checked.issues.length)
      throw Error(
          'userspace upgrade requires a clean root; shut down or run fsck first');
    let ino = 2;
    while (ino < limit && nodes.has(ino))
      ino++;
    if (ino === limit)
      throw Error('not enough free inodes to install userspace');
    const base = inodeBase(ino);
    image.fill(0, base * 2, (base + 32) * 2);
    const node = {ino, base, map : [], owned : [], size : 0};
    nodes.set(ino, node);
    put(base, mode);
    put(base + 1, links);
    changed = true;
    return node;
  }
  function setEntry(parent, name, ino, offset = -1) {
    const directory = cells(parent);
    if (offset < 0)
      offset = directory.findIndex((v, i) => i % 32 === 0 && !v);
    if (offset < 0) {
      offset = directory.length;
      directory.push(...Array(32).fill(0));
    }
    directory[offset] = ino;
    for (let j = 0; j < 30; j++)
      directory[offset + 1 + j] = name.charCodeAt(j) || 0;
    directory[offset + 31] = 0;
    writeNode(parent, directory);
  }

  for (const f of files) {
    const parts = f.name.split('/').filter(Boolean), name = parts.pop();
    if (!name || name.length > 30 || parts.some(n => n === '.' || n === '..'))
      throw Error('invalid install path');
    let parent = nodes.get(1);
    for (const part of parts) {
      let entry = lookup(parent, part);
      if (!entry) {
        const dir = newNode(part === 'tmp' ? 0o41777 : 0o40755, 2),
              data = Array(64).fill(0);
        data[0] = dir.ino;
        data[1] = 46;
        data[32] = parent.ino;
        data[33] = data[34] = 46;
        writeNode(dir, data);
        setEntry(parent, part, dir.ino);
        put(parent.base + 1, w(parent.base + 1) + 1);
        entry = {ino : dir.ino};
      }
      if ((w(nodes.get(entry.ino).base) & 0xf000) !== 0x4000)
        throw Error(`not an install directory: ${part}`);
      parent = nodes.get(entry.ino);
    }
    const entry = lookup(parent, name),
          old = entry ? nodes.get(entry.ino) : null;
    if (f.directory) {
      if (old) {
        if ((w(old.base) & 0xf000) !== 0x4000)
          throw Error(`refusing to replace directory path ${f.name}`);
        continue;
      }
      const dir = newNode(f.mode ?? 0o40755, 2), data = Array(64).fill(0);
      data[0] = dir.ino;
      data[1] = 46;
      data[32] = parent.ino;
      data[33] = data[34] = 46;
      writeNode(dir, data);
      setEntry(parent, name, dir.ino);
      put(parent.base + 1, w(parent.base + 1) + 1);
      continue;
    }
    if (f.device !== undefined) {
      if (old) {
        if (w(old.base) !== (f.mode ?? 0o20666) ||
            w(old.base + 12) !== f.device)
          throw Error(`refusing to replace existing device path ${f.name}`);
        continue;
      }
      const node = newNode(f.mode ?? 0o20666);
      put(node.base + 12, f.device);
      setEntry(parent, name, node.ino);
      continue;
    }
    if (old && (w(old.base) & 0xf000) !== 0x8000)
      throw Error(`refusing to replace nonregular ${f.name}`);
    if (old && f.preserve)
      continue;
    if (f.link) {
      const targetParts = f.link.split('/').filter(Boolean);
      let target = nodes.get(1);
      if (!targetParts.length || targetParts.some(n => n === '.' || n === '..'))
        throw Error('invalid install link target');
      for (const part of targetParts) {
        if ((w(target.base) & 0xf000) !== 0x4000)
          throw Error(`not an install link directory: ${f.link}`);
        const next = lookup(target, part);
        if (!next) throw Error(`missing install link target: ${f.link}`);
        target = nodes.get(next.ino);
      }
      if ((w(target.base) & 0xf000) !== 0x8000)
        throw Error(`not a regular install link target: ${f.link}`);
      if (old === target) continue;
      if (checked.issues.length)
        throw Error('userspace upgrade requires a clean root; shut down or run fsck first');
      if (old) {
        const links = w(old.base + 1) - 1;
        put(old.base + 1, links);
        if (!links) {
          writeNode(old, []);
          image.fill(0, old.base * 2, (old.base + 32) * 2);
          nodes.delete(old.ino);
        }
      }
      put(target.base + 1, w(target.base + 1) + 1);
      setEntry(parent, name, target.ino, entry ? entry.offset : -1);
      changed = true;
      continue;
    }
    const data = Array.from(f.data), mode = f.mode ?? 0o100755;
    if (old && w(old.base) === mode && old.size === data.length &&
        cells(old).every((c, i) => c === data[i]))
      continue;
    if (checked.issues.length)
      throw Error(
          'userspace upgrade requires a clean root; shut down or run fsck first');
    let node = old;
    // A user may have hard-linked an old binary; preserve their other path.
    if (!node || w(node.base + 1) > 1) {
      node = newNode(mode);
      if (old)
        put(old.base + 1, w(old.base + 1) - 1);
      setEntry(parent, name, node.ino, entry ? entry.offset : -1);
    }
    put(node.base, mode);
    writeNode(node, data);
    changed = true;
  }
  if (!changed)
    return input;
  // Rebuild free lists from the new ownership graph, then check the result
  // again.
  put((CELLS + 105), 1);
  const result = checkRoot(image, true).image;
  if (checkRoot(result).issues.length)
    throw Error('installed root failed validation');
  return result;
}

export function growRoot(input, blocks = 65535) {
  const oldBlocks = input.length / BLOCK, oldIsize = input.readUInt16LE((CELLS + 1) * 2),
        newIsize = Math.max(oldIsize, 2 + 1024 / INODES_PER_BLOCK);
  if (oldBlocks >= blocks && oldIsize === newIsize)
    return input;
  if (checkRoot(input).issues.length)
    throw Error('root expansion requires a clean filesystem');
  const image = Buffer.alloc(Math.max(oldBlocks, blocks) * BLOCK);
  input.copy(image);
  const w = p => image.readUInt16LE(p * 2),
        put = (p, v) => image.writeUInt16LE(v, p * 2), moved = new Map();
  // Make room for 1,024 inodes on old bootstrap roots. Move early data blocks
  // above the old filesystem end, preserving every existing inode number.
  if (oldBlocks + newIsize - oldIsize > image.length / BLOCK)
    throw Error('insufficient room to expand inode table');
  for (let b = oldIsize; b < newIsize; b++) {
    const target = oldBlocks + b - oldIsize;
    image.copy(image, target * BLOCK, b * BLOCK, (b + 1) * BLOCK);
    moved.set(b, target);
    image.fill(0, b * BLOCK, (b + 1) * BLOCK);
  }
  function pointer(at, level) {
    let b = w(at);
    if (!b)
      return;
    if (moved.has(b)) {
      b = moved.get(b);
      put(at, b);
    }
    if (level)
      for (let i = 0; i < CELLS; i++)
        pointer(b * CELLS + i, level - 1);
  }
  for (let ino = 1; ino < (oldIsize - 2) * INODES_PER_BLOCK; ino++) {
    const base = inodeBase(ino),
          type = w(base) & 0xf000;
    if (type !== 0x8000 && type !== 0x4000 && type !== 0x1000)
      continue;
    for (let j = 0; j < 18; j++)
      pointer(base + 12 + j, 0);
    pointer(base + 30, 1);
    pointer(base + 31, 2);
  }
  put((CELLS + 1), newIsize);
  put((CELLS + 2), image.length / BLOCK);
  put((CELLS + 105), 1);
  const result = checkRoot(image, true).image;
  if (checkRoot(result).issues.length)
    throw Error('expanded root failed verification');
  return result;
}
