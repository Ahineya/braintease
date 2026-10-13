#!/usr/bin/env node
// Doom asset transport for the existing 16-bit storage MMIO. Logical WAD
// offsets always count original octets; expanded/cooked store each octet in
// one little-endian u16 word. The source WAD is never modified.
import fs from 'node:fs';
import path from 'node:path';
import { constants as bufferConstants } from 'node:buffer';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

export const ASSET_VERSION = 2;
export const WAD_MODES = Object.freeze({ packed: 0, expanded: 1, cooked: 2 });
const HEADER_WORDS = 32;
const METADATA_ABI = 1;
const DIRECTORY_RECORD_WORDS = 18;
const HASH_ENTRY_WORDS = 1;
const PNAMES_ENTRY_WORDS = 1;
const SECTOR_OCTETS = 65536;
const MAX_DISK_WORDS = 0x80000000; // 65536 blocks, each containing 32768 words.
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const demand = (condition, message) => { if (!condition) throw new Error(message); };
export function checkMode(mode) {
  demand(Object.hasOwn(WAD_MODES, mode), 'WAD mode must be packed, expanded or cooked');
  return mode;
}
function range(data, offset, length, label) {
  demand(Number.isSafeInteger(offset) && Number.isSafeInteger(length) && offset >= 0
    && length >= 0 && offset <= data.length && length <= data.length - offset,
  `${label}: range outside data`);
}
// Match the guest's ASCII toupper exactly. Unicode case conversion changes
// high octets (and can expand one character into several), unlike Doom names.
const upperAscii = octet => octet >= 97 && octet <= 122 ? octet - 32 : octet;
function name8(bytes) {
  let name = '';
  for (const octet of bytes.subarray(0, 8)) {
    if (octet === 0) break;
    name += String.fromCharCode(upperAscii(octet));
  }
  return name;
}
function nameHash(name) {
  let value = 5381;
  for (let i = 0; i < name.length; ++i)
    value = (((value << 5) ^ value) ^ name.charCodeAt(i)) & 65535;
  return value;
}

export function wadDirectory(source) {
  range(source, 0, 12, 'WAD header');
  demand(['IWAD', 'PWAD'].includes(source.toString('ascii', 0, 4)), 'invalid WAD signature');
  const count = source.readUInt32LE(4), directory = source.readUInt32LE(8);
  demand(count > 0, 'empty WAD directory');
  range(source, directory, count * 16, 'WAD directory');
  return Array.from({ length: count }, (_, index) => {
    const pos = directory + index * 16;
    const offset = source.readUInt32LE(pos), length = source.readUInt32LE(pos + 4);
    const nameBytes = Buffer.from(source.subarray(pos + 8, pos + 16));
    const name = name8(nameBytes);
    range(source, offset, length, `WAD lump ${index} (${name})`);
    return { index, name, nameBytes, offset, length };
  });
}

// Validate every column/post before allocating output. Duplicate columns are
// legal. Payload octets are kept verbatim, including padding and unused bytes.
export function cookPatch(raw) {
  demand(raw.length <= 0x7fffffff, 'patch too large');
  range(raw, 0, 8, 'patch header');
  const width = raw.readUInt16LE(0), height = raw.readUInt16LE(2);
  demand(width > 0 && width <= 32767 && height > 0 && height <= 32767,
    'invalid patch dimensions');
  const directoryEnd = 8 + width * 4;
  range(raw, 0, directoryEnd, 'patch directory');
  const headerWords = 4 + 2 * Math.max(width, 8);
  const words = headerWords + raw.length - directoryEnd;
  demand(words <= 0x7fffffff && words * 2 <= bufferConstants.MAX_LENGTH, 'patch too large');
  const columns = [];
  for (let x = 0; x < width; ++x) {
    const column = raw.readUInt32LE(8 + x * 4);
    demand(column >= directoryEnd && column < raw.length, 'invalid patch column');
    let at = column;
    while (raw[at] !== 255) {
      range(raw, at, 4, 'patch post');
      const postSize = 4 + raw[at + 1];
      range(raw, at, postSize + 1, 'patch post and next marker');
      demand(raw[at] + raw[at + 1] <= height, 'patch post exceeds height');
      at += postSize;
    }
    columns.push(headerWords + column - directoryEnd);
  }
  const cooked = Buffer.alloc(words * 2);
  for (let i = 0; i < 4; ++i) cooked.writeUInt16LE(raw.readUInt16LE(i * 2), i * 2);
  columns.forEach((column, x) => cooked.writeUInt32LE(column, 8 + x * 4));
  for (let i = directoryEnd; i < raw.length; ++i)
    cooked.writeUInt16LE(raw[i], (headerWords + i - directoryEnd) * 2);
  return cooked;
}

function readPnames(source, entries) {
  const entry = entries.findLast(item => item.name === 'PNAMES');
  if (!entry) return { entry: null, names: [] };
  const pnames = source.subarray(entry.offset, entry.offset + entry.length);
  range(pnames, 0, 4, 'PNAMES header');
  const count = pnames.readUInt32LE(0);
  demand(count <= 32767, 'PNAMES has too many patch names');
  range(pnames, 4, count * 8, 'PNAMES names');
  return { entry, names: Array.from({ length: count }, (_, i) => name8(pnames.subarray(4 + i * 8, 12 + i * 8))) };
}

function requiredPatches(source, entries, names) {
  // Doom resolves the last occurrence of a name. Missing unused PNAMES entries
  // are legal; only actual texture references must resolve to a patch.
  const byName = new Map(entries.map(entry => [entry.name, entry]));
  const required = new Set();
  const lump = entry => source.subarray(entry.offset, entry.offset + entry.length);
  for (const name of ['TEXTURE1', 'TEXTURE2']) {
    if (!byName.has(name)) continue;
    const textures = lump(byName.get(name));
    range(textures, 0, 4, `${name} header`);
    const count = textures.readUInt32LE(0), tableEnd = 4 + count * 4;
    range(textures, 0, tableEnd, `${name} offsets`);
    for (let i = 0; i < count; ++i) {
      const pos = textures.readUInt32LE(4 + i * 4);
      demand(pos >= tableEnd, `${name}: texture overlaps offset table`);
      range(textures, pos, 22, `${name} texture header`);
      demand(textures.readUInt16LE(pos + 12) > 0 && textures.readUInt16LE(pos + 14) > 0,
        `${name}: invalid texture dimensions`);
      const patches = textures.readUInt16LE(pos + 20);
      range(textures, pos + 22, patches * 10, `${name} texture patches`);
      for (let p = 0; p < patches; ++p) {
        const index = textures.readUInt16LE(pos + 26 + p * 10);
        demand(index < names.length, `${name}: patch index outside PNAMES`);
        const patch = byName.get(names[index]);
        demand(patch, `${name}: missing referenced patch ${names[index]}`);
        required.add(patch.index);
      }
    }
  }
  let sprites = false;
  for (const entry of entries) {
    if (/^S(?:S|\d)?_START$/.test(entry.name)) { sprites = true; continue; }
    if (/^S(?:S|\d)?_END$/.test(entry.name)) { sprites = false; continue; }
    if (sprites && entry.length) required.add(entry.index);
    // These names are unambiguously patches in the maintained engine. Other
    // UI/extension names are cooked only if fully valid; index zero uses the
    // checked raw runtime decoder rather than silently dropping the asset.
    if (/^(?:TITLEPIC|INTERPIC|CREDIT|HELP[12]?|VICTORY2|PFUB[12]|END[0-6]|BOSSBACK|DMENUPIC)$/.test(entry.name)
        || /^M_/.test(entry.name) || /^ST(?:BAR|TPRCNT|TNUM\d|YSNUM\d|GNUM\d|KEYS\d|FB\d|F[A-Z0-9]+)$/.test(entry.name)
        || /^STCFN\d{3}$/.test(entry.name)) required.add(entry.index);
  }
  return required;
}

export function packAssets(source, mode = 'cooked') {
  checkMode(mode);
  demand(Buffer.isBuffer(source), 'source WAD must be a Buffer');
  const sourceHash = hash(source);
  const metadata = { mode, format_version: ASSET_VERSION, source_sha256: sourceHash,
    source_octets: source.length, header_word: 0, index_word: 0, total_words: 0,
    directory_word: 0, hash_heads_word: 0, pnames_word: 0, pnames_count: 0,
    pnames_lump: 0xffffffff, metadata_abi: METADATA_ABI,
    directory_record_words: DIRECTORY_RECORD_WORDS, hash_entry_words: HASH_ENTRY_WORDS,
    pnames_entry_words: PNAMES_ENTRY_WORDS,
    lump_count: 0, cooked_patches: 0, fingerprint_lo: 0, fingerprint_hi: 0 };
  const patches = [];
  let entries, pnames;
  let words = mode === 'packed' ? Math.ceil(source.length / 2) : source.length;
  if (mode === 'cooked') {
    entries = wadDirectory(source);
    demand(entries.length <= 32767, 'cooked WAD has too many lump IDs');
    pnames = readPnames(source, entries);
    const required = requiredPatches(source, entries, pnames.names);
    metadata.header_word = source.length;
    metadata.index_word = source.length + HEADER_WORDS;
    metadata.lump_count = entries.length;
    metadata.directory_word = metadata.index_word + entries.length * 4;
    metadata.hash_heads_word = metadata.directory_word + entries.length * DIRECTORY_RECORD_WORDS;
    metadata.pnames_word = metadata.hash_heads_word + entries.length * HASH_ENTRY_WORDS;
    metadata.pnames_count = pnames.names.length;
    metadata.pnames_lump = pnames.entry?.index ?? 0xffffffff;
    words = metadata.pnames_word + pnames.names.length * PNAMES_ENTRY_WORDS;
    for (const entry of entries) {
      let patch;
      try { patch = cookPatch(source.subarray(entry.offset, entry.offset + entry.length)); }
      catch (error) {
        if (required.has(entry.index)) throw new Error(`invalid required patch ${entry.name} (lump ${entry.index}): ${error.message}`);
        continue;
      }
      patches.push({ index: entry.index, word: words, data: patch });
      words += patch.length / 2;
    }
    metadata.cooked_patches = patches.length;
    // Fingerprint is SHA-256 bytes 0..3 interpreted as a little-endian u32.
    const fingerprint = Buffer.from(sourceHash, 'hex').readUInt32LE(0);
    metadata.fingerprint_lo = fingerprint & 65535;
    metadata.fingerprint_hi = fingerprint >>> 16;
  }
  demand(Number.isSafeInteger(words) && words <= MAX_DISK_WORDS, 'asset image exceeds storage address space');
  const payloadOctets = mode === 'packed' ? source.length : words * 2;
  const diskOctets = Math.ceil(payloadOctets / SECTOR_OCTETS) * SECTOR_OCTETS;
  demand(diskOctets <= bufferConstants.MAX_LENGTH, 'asset image exceeds host Buffer limit');
  const image = Buffer.alloc(diskOctets);
  if (mode === 'packed') source.copy(image);
  else {
    for (let i = 0; i < source.length; ++i) image.writeUInt16LE(source[i], i * 2);
    metadata.total_words = words;
  }
  if (mode === 'cooked') {
    const header = metadata.header_word * 2;
    for (const [i, value] of [0x5257, 0x4144, ASSET_VERSION, 1].entries()) image.writeUInt16LE(value, header + i * 2);
    for (const [i, value] of [source.length, metadata.lump_count, metadata.index_word, words,
      metadata.fingerprint_lo + metadata.fingerprint_hi * 65536].entries()) image.writeUInt32LE(value, header + 8 + i * 4);
    for (const [i, value] of [metadata.directory_word, metadata.hash_heads_word, metadata.pnames_word,
      metadata.pnames_count, metadata.pnames_lump].entries()) image.writeUInt32LE(value, header + 28 + i * 4);
    for (const [i, value] of [METADATA_ABI, DIRECTORY_RECORD_WORDS, HASH_ENTRY_WORDS, PNAMES_ENTRY_WORDS].entries())
      image.writeUInt16LE(value, header + 48 + i * 2);
    const heads = new Uint16Array(entries.length).fill(0xffff);
    for (const entry of entries) {
      const record = (metadata.directory_word + entry.index * DIRECTORY_RECORD_WORDS) * 2;
      for (let i = 0; i < 8; ++i) image.writeUInt16LE(entry.nameBytes[i], record + i * 2);
      // wad_file and cache pointers stay zero. The guest relocates the file
      // pointer and translates next's index (or 0xffff) into a far pointer.
      image.writeUInt32LE(entry.offset, record + 20);
      image.writeUInt32LE(entry.length, record + 24);
      const bucket = nameHash(entry.name) % entries.length;
      image.writeUInt16LE(heads[bucket], record + 32);
      heads[bucket] = entry.index;
    }
    for (let i = 0; i < heads.length; ++i) image.writeUInt16LE(heads[i], (metadata.hash_heads_word + i) * 2);
    const byName = new Map(entries.map(entry => [entry.name, entry.index]));
    pnames.names.forEach((name, i) => image.writeUInt16LE(byName.get(name) ?? 0xffff, (metadata.pnames_word + i) * 2));
    for (const patch of patches) {
      const index = metadata.index_word * 2 + patch.index * 8;
      image.writeUInt32LE(patch.word, index);
      image.writeUInt32LE(patch.data.length / 2, index + 4);
      patch.data.copy(image, patch.word * 2);
    }
  }
  Object.assign(metadata, { octets: image.length, padding_octets: image.length - payloadOctets, sha256: hash(image) });
  return { image, metadata };
}

export function assertSeparateFile(destination, sources) {
  // Resolve existing ancestors too, so a new cache path has exactly the same
  // spelling on its first build and subsequent cache hits (notably /var on Mac).
  let ancestor = path.resolve(destination);
  const suffix = [];
  while (!fs.existsSync(ancestor)) {
    suffix.unshift(path.basename(ancestor));
    ancestor = path.dirname(ancestor);
  }
  const target = path.join(fs.realpathSync(ancestor), ...suffix);
  for (const source of sources) {
    const original = fs.realpathSync(source);
    demand(target !== original, 'the VM disk image must differ from the source WAD and cached asset image');
    if (fs.existsSync(target)) {
      const a = fs.statSync(original), b = fs.statSync(target);
      demand(a.dev !== b.dev || a.ino !== b.ino, 'the VM disk image must not be a hard link to the source WAD or cached asset image');
    }
  }
  return target;
}

export function writeAtomic(file, bytes) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, bytes);
  fs.renameSync(temporary, file);
}

export function cachedAssets(sourcePath, destination, mode = 'cooked') {
  checkMode(mode);
  const source = fs.readFileSync(sourcePath), sourceHash = hash(source);
  const generatorHash = hash(fs.readFileSync(fileURLToPath(import.meta.url)));
  const file = assertSeparateFile(destination, [sourcePath]), sidecar = `${file}.json`;
  // Reject source aliases at both outputs, including symlinks and hard links.
  assertSeparateFile(sidecar, [sourcePath]);
  if (fs.existsSync(file) && fs.existsSync(sidecar)) {
    let cached;
    try { cached = JSON.parse(fs.readFileSync(sidecar, 'utf8')); } catch { /* Rebuild an incomplete cache. */ }
    if (cached?.mode === mode && cached.format_version === ASSET_VERSION && cached.source_sha256 === sourceHash
        && cached.generator_sha256 === generatorHash && hash(fs.readFileSync(file)) === cached.sha256)
      return { ...cached, path: file };
  }
  const { image, metadata } = packAssets(source, mode);
  const result = { ...metadata, path: file, generator_sha256: generatorHash };
  writeAtomic(file, image);
  writeAtomic(sidecar, `${JSON.stringify(result, null, 2)}\n`);
  return result;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [source, destination, mode = 'cooked', ...extra] = process.argv.slice(2);
    demand(source && destination && !extra.length, 'usage: assets-pack.mjs SOURCE.wad OUTPUT.img [packed|expanded|cooked]');
    process.stdout.write(`${JSON.stringify(cachedAssets(source, destination, mode), null, 2)}\n`);
  } catch (error) { console.error(`error: ${error.message}`); process.exitCode = 1; }
}
