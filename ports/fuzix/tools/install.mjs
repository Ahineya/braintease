// Install managed userspace files into a clean C16 root, preserving other
// inodes.
import {checkRoot} from './fsck.mjs';
import {BLOCK, CELLS, INODES_PER_BLOCK, INODE_CELLS, ROOT_BLOCKS, MAGIC, SIGNATURE, SB, inodeBase, blockPath} from './layout.mjs';
import {Image} from './image.mjs';

export function installRoot(input, files) {
  const checked = checkRoot(input);
  if (checked.format.version !== 3) throw Error('upgrade C16 V2 with growRoot before installing');
  const image = checked.image, w = p => image.readUInt16LE(p * 2),
        put = (p, v) => image.writeUInt16LE(v, p * 2),
        wide = p => image.readUInt32LE(p * 2), putWide = (p, v) => image.writeUInt32LE(v, p * 2);
  const isize = checked.isize, blocks = checked.blocks, limit = (isize - 2) * INODES_PER_BLOCK;
  const nodes = checked.nodes, used = new Set(checked.owners.keys());
  for (const node of nodes.values()) node.owned = [];
  for (const [block, ino] of checked.owners) nodes.get(ino).owned.push(block);
  function cells(node) {
    const result = new Uint16Array(node.size);
    for (const [index, block] of node.map) {
      const chunk = image.subarray(block * BLOCK, (block + 1) * BLOCK);
      for (let i = 0; i < CELLS && index * CELLS + i < node.size; i++)
        result[index * CELLS + i] = chunk.readUInt16LE(i * 2);
    }
    return result;
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
    for (const b of node.owned) { used.delete(b); if (b < cursor) cursor = b; }
    node.owned = []; node.map = new Map();
    image.fill(0, (node.base + 12) * 2, (node.base + INODE_CELLS) * 2);
    const block = () => { const b = alloc(); node.owned.push(b); return b; };
    for (let i = 0; i < Math.ceil(data.length / CELLS); i++) {
      const b = block(); node.map.set(i, b);
      installBlock(image, node.base, i, b, block);
      const chunk = Buffer.alloc(BLOCK);
      for (let j = 0; j < Math.min(CELLS, data.length - i * CELLS); j++) chunk.writeUInt16LE(data[i * CELLS + j], j * 2);
      image.set(chunk, b * BLOCK);
    }
    node.size = data.length; putWide(node.base + 4, data.length);
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
    image.fill(0, base * 2, (base + INODE_CELLS) * 2);
    const node = {ino, base, map : new Map(), owned : [], size : 0};
    nodes.set(ino, node);
    put(base, mode);
    put(base + 1, links);
    changed = true;
    return node;
  }
  function setEntry(parent, name, ino, offset = -1) {
    const directory = Array.from(cells(parent));
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
        const mode = f.mode ?? 0o20666, device = w(old.base + 12);
        if (w(old.base) !== mode ||
            (device !== f.device && device !== f.migrateDeviceFrom))
          throw Error(`refusing to replace existing device path ${f.name}`);
        if (device === f.device)
          continue;
        if (checked.issues.length)
          throw Error('userspace upgrade requires a clean root; shut down or run fsck first');
        let node = old;
        // Migrate only the managed path, retaining any user-created aliases
        // to the old device and preserving the node's other metadata.
        if (w(old.base + 1) > 1) {
          node = newNode(mode);
          image.copy(image, node.base * 2, old.base * 2, (old.base + INODE_CELLS) * 2);
          put(node.base + 1, 1);
          put(old.base + 1, w(old.base + 1) - 1);
          setEntry(parent, name, node.ino, entry.offset);
        }
        put(node.base + 12, f.device);
        changed = true;
        continue;
      }
      const node = newNode(f.mode ?? 0o20666);
      put(node.base + 12, f.device);
      setEntry(parent, name, node.ino);
      continue;
    }
    if (old && ![0x8000, 0xa000].includes(w(old.base) & 0xf000))
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
          image.fill(0, old.base * 2, (old.base + INODE_CELLS) * 2);
          nodes.delete(old.ino);
        }
      }
      put(target.base + 1, w(target.base + 1) + 1);
      setEntry(parent, name, target.ino, entry ? entry.offset : -1);
      changed = true;
      continue;
    }
    const symbolic = f.symlink !== undefined;
    if (symbolic && (typeof f.symlink !== 'string' || !f.symlink.length ||
        f.symlink.length >= 512 || f.symlink.includes('\0')))
      throw Error('invalid install symbolic link target');
    const data = symbolic ? f.symlink.split('').map(c => c.charCodeAt(0)) : f.data,
          mode = symbolic ? 0o120777 : (f.mode ?? 0o100755);
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
  put((CELLS + SB.clean), 1);
  const result = checkRoot(image, true).image;
  if (checkRoot(result).issues.length)
    throw Error('installed root failed validation');
  return result;
}

// Install one logical data block, allocating only the necessary pointer path.
export function installBlock(image, base, index, block, allocate) {
  const path = blockPath(index);
  let at = base + 12 + path[0] * 2;
  for (const slot of path.slice(1)) {
    let pointer = image.readUInt32LE(at * 2);
    if (!pointer) { pointer = allocate(); image.writeUInt32LE(pointer, at * 2); }
    at = pointer * CELLS + slot * 2;
  }
  image.writeUInt32LE(block, at * 2);
}

export function growRoot(input, blocks = ROOT_BLOCKS, inodeCount = 1024) {
  if (!Number.isInteger(inodeCount) || inodeCount < 16 || inodeCount > 65520 || inodeCount % INODES_PER_BLOCK)
    throw Error('invalid inode count');
  const old = checkRoot(input), count = Math.max(inodeCount, (old.isize - 2) * old.format.inodes);
  if (!Number.isInteger(blocks) || blocks < 1 || blocks * BLOCK > 0xffffffff) throw Error('invalid root size');
  if (old.format.version === 3 && old.blocks >= blocks && old.format.inodes * (old.isize - 2) >= count) return input;
  if (old.issues.length) throw Error('root expansion requires a clean filesystem');
  // Repack blocks, never inode numbers. This preserves hard links, ownership,
  // timestamps, native characters and holes, including across V2/V3 tree shapes.
  const image = new Image(Math.max(old.blocks, blocks) * BLOCK), isize = 2 + count / INODES_PER_BLOCK;
  image.write(SIGNATURE, 0, 'ascii'); image.writeUInt16LE(MAGIC, CELLS * 2);
  image.writeUInt16LE(isize, (CELLS + SB.isize) * 2);
  image.writeUInt32LE(image.length / BLOCK, (CELLS + SB.blocks) * 2);
  image.writeUInt32LE(old.image.readUInt32LE((CELLS + old.format.sb.time) * 2), (CELLS + SB.time) * 2);
  image.writeUInt16LE(0o100000, inodeBase(0) * 2); image.writeUInt16LE(1, (inodeBase(0) + 1) * 2);
  let cursor = isize;
  function allocate() {
    if (cursor >= image.length / BLOCK) throw Error('not enough space to upgrade root');
    return cursor++;
  }
  for (const node of old.nodes.values()) {
    const base = inodeBase(node.ino);
    old.image.copy(image, base * 2, node.base * 2, (node.base + 12) * 2);
    if (node.type === 0x2000 || node.type === 0x6000)
      image.writeUInt32LE(old.format.ptr(node.base + 12), (base + 12) * 2);
    for (const [index, source] of [...node.map].sort((a, b) => a[0] - b[0])) {
      const block = allocate();
      installBlock(image, base, index, block, allocate);
      old.image.copy(image, block * BLOCK, source * BLOCK, (source + 1) * BLOCK);
    }
  }
  const result = checkRoot(image, true).image;
  if (checkRoot(result).issues.length) throw Error('upgraded root failed verification');
  return result;
}
