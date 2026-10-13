// Final sprite rotation tables for the 16-bit-cell renderer. These tools run
// while producing/checking cooked assets; the VM sees only ordinary disk data.
import fs from 'node:fs';
import { createHash } from 'node:crypto';

export const SPRITE_HEADER_WORDS = 16;
export const SPRITE_DEFINITION_WORDS = 3;
export const SPRITE_FRAME_WORDS = 17;
const demand = (condition, text) => { if (!condition) throw new Error('Invalid sprite metadata: ' + text); };
const sha256 = data => createHash('sha256').update(data).digest('hex');
const upper = value => value >= 97 && value <= 122 ? value - 32 : value;
const key4 = bytes => Array.from(bytes.subarray(0, 4), upper).join(',');

export function readSpriteNames() {
  const source = fs.readFileSync(new URL('../engine/info.c', import.meta.url), 'utf8');
  const body = source.match(/char\s*\*\s*sprnames\s*\[\s*\]\s*=\s*\{([^}]+)\}\s*;/)?.[1];
  demand(body && /^\s*(?:"[A-Z0-9]{4}"\s*,\s*)+NULL\s*$/.test(body),
    'unsupported sprnames initializer; update the asset generator explicitly');
  return [...body.matchAll(/"([A-Z0-9]{4})"/g)].map(match => match[1]);
}

export function spriteNameIdentity(names = readSpriteNames()) {
  demand(Array.isArray(names) && names.length > 0 && names.length <= 32767
    && names.every(name => typeof name === 'string' && /^[A-Z0-9]{4}$/.test(name))
    && new Set(names).size === names.length, 'invalid sprite name list');
  const namesSha256 = sha256(names.join('\0') + '\0');
  const fingerprint = Buffer.from(namesSha256, 'hex').readUInt32LE(0);
  return { namesSha256, namesLo: fingerprint & 65535, namesHi: fingerprint >>> 16 };
}

function spriteRange(entries) {
  demand(Array.isArray(entries) && entries.length <= 32767, 'invalid WAD directory');
  const start = entries.findLastIndex(entry => entry.name === 'S_START');
  const end = entries.findLastIndex(entry => entry.name === 'S_END');
  if (start < 0 && end < 0) return null;
  demand(start >= 0 && end > start, 'missing or reversed sprite markers');
  entries.forEach((entry, index) => demand(entry.index === index
    && Buffer.isBuffer(entry.nameBytes) && entry.nameBytes.length === 8, 'invalid raw lump name'));
  return { firstLump: start + 1, lastLump: end - 1 };
}

export function cookSpriteMetadata(entries, names = readSpriteNames()) {
  const identity = spriteNameIdentity(names), range = spriteRange(entries);
  if (!range) return null;
  const grouped = new Map(names.map(name => [key4(Buffer.from(name)), []]));
  for (let lump = range.firstLump; lump <= range.lastLump; ++lump) {
    const bytes = entries[lump].nameBytes;
    const definitions = grouped.get(key4(bytes));
    if (!definitions) continue;
    for (const slot of bytes[6] ? [4, 6] : [4]) {
      const frame = bytes[slot] - 65, rotation = bytes[slot + 1] - 48;
      demand(frame >= 0 && frame < 29 && rotation >= 0 && rotation <= 8,
        'bad frame characters in lump ' + lump);
      while (definitions.length <= frame) definitions.push(null);
      const current = definitions[frame] ??= { rotate: -1, lump: Array(8).fill(-1), flip: Array(8).fill(-1) };
      demand(rotation === 0 ? current.rotate === -1 : current.rotate !== 0,
        'duplicate or mixed rotation-zero frame in lump ' + lump);
      current.rotate = rotation === 0 ? 0 : 1;
      for (const direction of rotation === 0 ? [0, 1, 2, 3, 4, 5, 6, 7] : [rotation - 1]) {
        demand(current.lump[direction] === -1, 'duplicate sprite rotation in lump ' + lump);
        current.lump[direction] = lump - range.firstLump;
        current.flip[direction] = slot === 6 ? 1 : 0;
      }
    }
  }
  const definitions = names.map(name => grouped.get(key4(Buffer.from(name))));
  definitions.forEach((frames, index) => frames.forEach(frame => demand(frame && !frame.lump.includes(-1),
    'missing frame or rotation for ' + names[index])));
  const frames = definitions.reduce((sum, entry) => sum + entry.length, 0);
  const data = Buffer.alloc((SPRITE_HEADER_WORDS + names.length * SPRITE_DEFINITION_WORDS
    + frames * SPRITE_FRAME_WORDS) * 2);
  const put = (word, value) => data.writeUInt16LE(value, word * 2);
  [0x5350, 1, names.length, SPRITE_FRAME_WORDS, frames & 65535, frames >>> 16,
    range.firstLump, range.lastLump, identity.namesLo, identity.namesHi, SPRITE_DEFINITION_WORDS]
    .forEach((value, word) => put(word, value));
  let at = SPRITE_HEADER_WORDS + names.length * SPRITE_DEFINITION_WORDS;
  definitions.forEach((sprite, index) => {
    put(SPRITE_HEADER_WORDS + index * SPRITE_DEFINITION_WORDS, sprite.length);
    for (const frame of sprite)
      for (const value of [frame.rotate, ...frame.lump, ...frame.flip]) put(at++, value);
  });
  return { data, count: names.length, frames, ...range, ...identity };
}

// Independent readback: derive each frame's eight source candidates directly
// from the WAD directory, without reusing the packer's state/install routine.
export function validateSpriteMetadata(data, entries, names = readSpriteNames()) {
  const identity = spriteNameIdentity(names), range = spriteRange(entries);
  if (data == null || Buffer.isBuffer(data) && data.length === 0) {
    demand(range === null, 'missing section for sprite markers');
    return { count: 0, frames: 0 };
  }
  demand(range !== null, 'section without sprite markers');
  demand(Buffer.isBuffer(data) && data.length % 2 === 0
    && data.length >= (SPRITE_HEADER_WORDS + names.length * SPRITE_DEFINITION_WORDS) * 2,
  'truncated section');
  const get = word => data.readUInt16LE(word * 2);
  const total = data.readUInt32LE(8);
  const expectedHeader = [0x5350, 1, names.length, SPRITE_FRAME_WORDS, total & 65535, total >>> 16,
    range.firstLump, range.lastLump, identity.namesLo, identity.namesHi, SPRITE_DEFINITION_WORDS, 0, 0, 0, 0, 0];
  expectedHeader.forEach((value, word) => demand(get(word) === value, 'header word ' + word));
  demand(total <= names.length * 29 && data.length === (SPRITE_HEADER_WORDS
    + names.length * SPRITE_DEFINITION_WORDS + total * SPRITE_FRAME_WORDS) * 2, 'section length');
  let at = SPRITE_HEADER_WORDS + names.length * SPRITE_DEFINITION_WORDS, checkedFrames = 0;
  names.forEach((name, index) => {
    const candidates = [];
    for (let lump = range.firstLump; lump <= range.lastLump; ++lump) {
      const bytes = entries[lump].nameBytes;
      if (![0, 1, 2, 3].every(i => upper(bytes[i]) === name.charCodeAt(i))) continue;
      for (let pair = 0; pair < (bytes[6] ? 2 : 1); ++pair) {
        const frame = bytes[4 + pair * 2] - 65, rotation = bytes[5 + pair * 2] - 48;
        demand(frame >= 0 && frame < 29 && rotation >= 0 && rotation <= 8, 'bad source frame characters');
        candidates.push({ frame, rotation, lump: lump - range.firstLump, flip: pair });
      }
    }
    const count = candidates.reduce((max, entry) => Math.max(max, entry.frame + 1), 0);
    const definition = SPRITE_HEADER_WORDS + index * SPRITE_DEFINITION_WORDS;
    demand(get(definition) === count && get(definition + 1) === 0 && get(definition + 2) === 0,
      'definition ' + name);
    demand(checkedFrames + count <= total, 'frame count exceeds section');
    for (let frame = 0; frame < count; ++frame) {
      const records = candidates.filter(entry => entry.frame === frame);
      const zero = records.filter(entry => entry.rotation === 0);
      demand(records.length && (!zero.length || records.length === 1), 'missing or mixed source frame');
      demand(get(at) === (zero.length ? 0 : 1), 'rotation flag for ' + name);
      for (let direction = 0; direction < 8; ++direction) {
        const matching = zero.length ? zero : records.filter(entry => entry.rotation === direction + 1);
        demand(matching.length === 1, 'missing or duplicate source rotation');
        demand(get(at + 1 + direction) === matching[0].lump
          && get(at + 9 + direction) === matching[0].flip, 'rotation mapping for ' + name);
      }
      at += SPRITE_FRAME_WORDS;
    }
    checkedFrames += count;
  });
  demand(checkedFrames === total, 'unused frame records');
  return { count: names.length, frames: total, ...range, ...identity };
}
