import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { ASSET_VERSION, packAssets, cookPatch, wadDirectory, cachedAssets } from './assets-pack.mjs';

const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
function temporary(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'doom-assets-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}
function putName(target, name, offset) {
  if (Buffer.isBuffer(name)) name.copy(target, offset, 0, 8);
  else target.write(name, offset, 8, 'latin1');
}
function wad(lumps, { signature = 'PWAD', gap = 0 } = {}) {
  const start = 12 + gap;
  const directory = start + lumps.reduce((length, lump) => length + lump.data.length, 0);
  const result = Buffer.alloc(directory + lumps.length * 16);
  result.write(signature); result.writeUInt32LE(lumps.length, 4); result.writeUInt32LE(directory, 8);
  let at = start;
  lumps.forEach((lump, i) => {
    const entry = directory + i * 16;
    result.writeUInt32LE(at, entry); result.writeUInt32LE(lump.data.length, entry + 4);
    putName(result, lump.name, entry + 8); lump.data.copy(result, at); at += lump.data.length;
  });
  return result;
}
function patch(width = 1, height = 3) {
  const directoryEnd = 8 + 4 * width;
  const result = Buffer.alloc(directoryEnd + 6);
  result.writeUInt16LE(width, 0); result.writeUInt16LE(height, 2);
  result.writeInt16LE(-3, 4); result.writeInt16LE(-7, 6);
  for (let x = 0; x < width; ++x) result.writeUInt32LE(directoryEnd, 8 + x * 4);
  result.set([1, 1, 17, 254, 29, 255], directoryEnd);
  return result;
}
function pnames(names) {
  const result = Buffer.alloc(4 + names.length * 8); result.writeUInt32LE(names.length);
  names.forEach((name, i) => putName(result, name, 4 + i * 8));
  return result;
}
function texture(index) {
  const result = Buffer.alloc(40); result.writeUInt32LE(1); result.writeUInt32LE(8, 4);
  result.write('TEXTURE', 8); result.writeUInt16LE(1, 20); result.writeUInt16LE(3, 22);
  result.writeUInt16LE(1, 28); result.writeUInt16LE(index, 34); return result;
}
function textureTable(definitions) {
  const table = 4 + definitions.length * 4;
  const result = Buffer.alloc(table + definitions.reduce((sum, item) => sum + 22 + item.patches.length * 10, 0));
  result.writeUInt32LE(definitions.length);
  let at = table;
  definitions.forEach((item, i) => {
    result.writeUInt32LE(at, 4 + i * 4); putName(result, item.name, at);
    result.writeUInt16LE(item.width, at + 12); result.writeUInt16LE(item.height, at + 14);
    result.writeUInt16LE(item.patches.length, at + 20);
    item.patches.forEach((part, j) => {
      result.writeInt16LE(part.x ?? 0, at + 22 + j * 10);
      result.writeInt16LE(part.y ?? 0, at + 24 + j * 10);
      result.writeUInt16LE(part.index ?? 0, at + 26 + j * 10);
    });
    at += 22 + item.patches.length * 10;
  });
  return result;
}
function columnPatch(width, height = 3) {
  const directory = 8 + width * 4, result = Buffer.alloc(directory + width * 6);
  result.writeUInt16LE(width); result.writeUInt16LE(height, 2);
  for (let x = 0; x < width; ++x) {
    result.writeUInt32LE(directory + x * 6, 8 + x * 4);
    result.set([0, 1, 0, (x + 17) & 255, 0, 255], directory + x * 6);
  }
  return result;
}
function decodedTextures({ image, metadata }) {
  const blob = image.subarray(metadata.texture_word * 2, (metadata.texture_word + metadata.texture_words) * 2);
  const word = offset => blob.readUInt16LE(offset * 2), wide = offset => blob.readUInt32LE(offset * 2);
  const count = word(2), definitions = [], columns = wide(6), definitionWords = wide(4);
  let at = 16;
  for (let i = 0; i < count; ++i) {
    const partCount = word(at + 13), parts = [];
    for (let p = 0; p < partCount; ++p) parts.push({
      x: blob.readInt16LE((at + 14 + p * 3) * 2),
      y: blob.readInt16LE((at + 15 + p * 3) * 2), lump: word(at + 16 + p * 3),
    });
    definitions.push({ name: Buffer.from(Array.from({ length: 8 }, (_, n) => word(at + n))),
      width: word(at + 8), height: word(at + 9), index: word(at + 10), next: word(at + 11),
      nextHigh: word(at + 12), parts });
    at += 14 + partCount * 3;
  }
  assert.equal(at, 16 + definitionWords, 'definitions consume exactly their declared region');
  const lumps = Array.from({ length: columns }, (_, i) => word(at + i)); at += columns;
  const offsets = Array.from({ length: columns }, (_, i) => wide(at + i * 2)); at += columns * 2;
  const masks = Array.from({ length: count }, (_, i) => word(at + i)); at += count;
  const heights = Array.from({ length: count }, (_, i) => wide(at + i * 2)); at += count * 2;
  const sizes = Array.from({ length: count }, (_, i) => wide(at + i * 2)); at += count * 2;
  const heads = Array.from({ length: count }, (_, i) => word(at + i)); at += count;
  assert.equal(at, metadata.texture_words, 'parallel arrays consume exactly the blob');
  assert.deepEqual([word(0), word(1), word(3)], [0x5458, 1, 0]);
  assert.ok(Array.from({ length: 5 }, (_, i) => word(11 + i)).every(n => n === 0));
  return { definitions, columns, definitionWords, lumps, offsets, masks, heights, sizes, heads,
    sourceIds: [word(8), word(9), word(10)] };
}
function support(...args) {
  return spawnSync(process.execPath, [path.join(port, 'tools/build-support.mjs'), ...args], { encoding: 'utf8' });
}

test('packed compatibility and expanded odd bytes preserve all source octets', () => {
  const source = Buffer.from([0, 128, 255, 1, 42]);
  const packed = packAssets(source, 'packed'), expanded = packAssets(source, 'expanded');
  assert.equal(packed.image.length, 65536);
  assert.deepEqual(packed.image.subarray(0, 5), source);
  assert.ok(packed.image.subarray(5).every(byte => byte === 0));
  assert.equal(packed.metadata.padding_octets, 65531);
  assert.equal(packed.metadata.total_words, 0);
  assert.deepEqual([...expanded.image.subarray(0, 10)], [0, 0, 128, 0, 255, 0, 1, 0, 42, 0]);
  assert.equal(expanded.metadata.total_words, 5);
  assert.equal(expanded.metadata.padding_octets, 65526);
  assert.ok(expanded.image.subarray(10).every(byte => byte === 0));
  assert.throws(() => packAssets(source, 'other'), /mode/);
});

test('transport crosses storage block boundaries and pads only at the end', () => {
  const source = Buffer.alloc(32771);
  for (let i = 0; i < source.length; ++i) source[i] = i % 251;
  const { image, metadata } = packAssets(source, 'expanded');
  assert.equal(image.length, 131072);
  for (let i = 0; i < source.length; ++i) assert.equal(image.readUInt16LE(i * 2), source[i]);
  assert.equal(metadata.padding_octets, 131072 - source.length * 2);
});

test('cooked small and wide patches exactly match Ripple cell layout', () => {
  for (const width of [1, 8, 9, 320]) {
    const raw = patch(width), converted = cookPatch(raw), header = 4 + 2 * Math.max(width, 8);
    assert.equal(converted.length, (header + 6) * 2);
    assert.deepEqual([...converted.subarray(0, 8)], [...raw.subarray(0, 8)]);
    for (let x = 0; x < width; ++x) assert.equal(converted.readUInt32LE(8 + x * 4), header);
    for (let x = width; x < 8; ++x) assert.equal(converted.readUInt32LE(8 + x * 4), 0);
    assert.deepEqual(Array.from({ length: 6 }, (_, i) => converted.readUInt16LE((header + i) * 2)),
      [1, 1, 17, 254, 29, 255]);
  }
});

test('every patch column and post is validated, including later columns', () => {
  assert.throws(() => cookPatch(Buffer.alloc(7)), /header/);
  for (const field of [0, 2]) for (const value of [0, 32768, 65535]) {
    const raw = patch(); raw.writeUInt16LE(value, field);
    assert.throws(() => cookPatch(raw), /dimensions/);
  }
  const missing = patch().subarray(0, -1);
  assert.throws(() => cookPatch(missing), /post/);
  const overlap = patch(); overlap.writeUInt32LE(8, 8);
  assert.throws(() => cookPatch(overlap), /column/);
  const outside = patch(); outside.writeUInt32LE(0xffffffff, 8);
  assert.throws(() => cookPatch(outside), /column/);
  const tall = patch(); tall[12] = 3;
  assert.throws(() => cookPatch(tall), /height/);
  const truncated = patch(); truncated[13] = 255;
  assert.throws(() => cookPatch(truncated), /post/);
  const later = patch(2); later.writeUInt32LE(later.length, 12);
  assert.throws(() => cookPatch(later), /column/);
});

test('distinct columns, multiple posts and unused payload retain exact offsets', () => {
  const raw = Buffer.alloc(31);
  raw.writeUInt16LE(2, 0); raw.writeUInt16LE(5, 2);
  raw.writeUInt32LE(16, 8); raw.writeUInt32LE(27, 12);
  raw.set([0, 1, 0, 4, 0, 3, 1, 0, 9, 0, 255, 255, 7, 8, 9], 16);
  const cooked = cookPatch(raw);
  assert.equal(cooked.readUInt32LE(8), 20);
  assert.equal(cooked.readUInt32LE(12), 31);
  assert.deepEqual(Array.from({ length: 15 }, (_, i) => cooked.readUInt16LE((20 + i) * 2)), [...raw.subarray(16)]);
});

test('cooked WAD retains original offsets and deterministic versioned index', () => {
  const rawPatch = patch();
  const source = wad([{ name: 'TITLEPIC', data: rawPatch }, { name: 'OTHER', data: Buffer.from([7, 2, 8]) }], { gap: 1 });
  const { image, metadata } = packAssets(source, 'cooked');
  assert.deepEqual(packAssets(source, 'cooked').image, image);
  for (let i = 0; i < source.length; ++i) assert.equal(image.readUInt16LE(i * 2), source[i]);
  const h = source.length * 2, index = (source.length + 44) * 2;
  assert.deepEqual(Array.from({ length: 4 }, (_, i) => image.readUInt16LE(h + i * 2)), [0x5257, 0x4144, ASSET_VERSION, 1]);
  assert.equal(image.readUInt32LE(h + 8), source.length);
  assert.equal(image.readUInt32LE(h + 12), 2);
  assert.equal(image.readUInt32LE(h + 16), index / 2);
  assert.equal(image.readUInt32LE(h + 20), metadata.total_words);
  assert.equal(image.readUInt32LE(h + 24), Buffer.from(sha(source), 'hex').readUInt32LE(0));
  assert.equal(image.readUInt32LE(h + 28), metadata.directory_word);
  assert.equal(image.readUInt32LE(h + 32), metadata.hash_heads_word);
  assert.equal(image.readUInt32LE(h + 36), metadata.pnames_word);
  assert.equal(image.readUInt32LE(h + 40), 0);
  assert.equal(image.readUInt32LE(h + 44), 0xffffffff);
  assert.deepEqual(Array.from({ length: 8 }, (_, i) => image.readUInt16LE(h + 48 + i * 2)), [1, 18, 1, 1, 0, 0, 0, 0]);
  assert.equal(metadata.index_word, metadata.header_word + 44);
  assert.equal(metadata.directory_word, metadata.index_word + 4 * metadata.lump_count);
  assert.equal(metadata.hash_heads_word, metadata.directory_word + 18 * metadata.lump_count);
  assert.equal(metadata.pnames_word, metadata.hash_heads_word + metadata.lump_count);
  assert.equal(image.readUInt32LE(h + 64), 0);
  assert.equal(image.readUInt32LE(h + 68), 0);
  assert.equal(image.readUInt32LE(h + 72), metadata.renderer_word);
  assert.equal(image.readUInt32LE(h + 76), metadata.renderer_words);
  assert.equal(metadata.renderer_word, metadata.pnames_word);
  const start = image.readUInt32LE(index), count = image.readUInt32LE(index + 4);
  assert.equal(start, metadata.renderer_word + metadata.renderer_words);
  assert.deepEqual(image.subarray(start * 2, (start + count) * 2), cookPatch(rawPatch));
  assert.equal(image.readBigUInt64LE(index + 8), 0n);
  assert.equal(metadata.cooked_patches, 1);
  assert.equal(metadata.sha256, sha(image));
});

test('WAD bounds reject malformed headers, directories and lumps without overflow', () => {
  assert.throws(() => packAssets(Buffer.alloc(11)), /header/);
  assert.throws(() => packAssets(Buffer.alloc(12)), /signature/);
  const source = wad([{ name: 'THING', data: Buffer.from([1]) }]);
  for (const [at, value] of [[4, 0xffffffff], [8, 0xfffffff8], [13, 0xffffffff], [17, 0xffffffff]]) {
    const bad = Buffer.from(source); bad.writeUInt32LE(value, at);
    assert.throws(() => packAssets(bad), /range/);
  }
  assert.throws(() => wadDirectory(wad([])), /empty WAD/);
});

test('texture references require patches, while missing unused PNAMES are legal', () => {
  const entries = [{ name: 'PNAMES', data: pnames(['ABSENT', 'PATCH']) },
    { name: 'TEXTURE1', data: texture(1) }, { name: 'PATCH', data: patch() }];
  assert.equal(packAssets(wad(entries)).metadata.cooked_patches, 1);
  // Even an existing malformed PNAMES entry is harmless when no texture uses it.
  assert.equal(packAssets(wad([...entries, { name: 'ABSENT', data: Buffer.alloc(3) }])).metadata.cooked_patches, 1);
  assert.throws(() => packAssets(wad([{ ...entries[0] }, { name: 'TEXTURE1', data: texture(0) }, entries[2]])), /missing referenced patch ABSENT/);
  assert.throws(() => packAssets(wad([{ ...entries[0] }, { name: 'TEXTURE1', data: texture(2) }, entries[2]])), /outside PNAMES/);
  assert.throws(() => packAssets(wad([entries[0], entries[1], { name: 'PATCH', data: Buffer.alloc(7) }])), /required patch PATCH/);
  const brokenTexture = texture(1); brokenTexture.writeUInt32LE(0xffffffff, 4);
  assert.throws(() => packAssets(wad([entries[0], { name: 'TEXTURE1', data: brokenTexture }, entries[2]])), /texture header/);
});

test('sprite and named UI corruption fails; unknown raw assets keep a zero index', () => {
  assert.throws(() => packAssets(wad([{ name: 'S_START', data: Buffer.alloc(0) },
    { name: 'TROOA1', data: Buffer.alloc(4) }, { name: 'S_END', data: Buffer.alloc(0) }])), /required patch TROOA1/);
  for (const name of ['TITLEPIC', 'M_DOOM', 'STBAR', 'STCFN033'])
    assert.throws(() => packAssets(wad([{ name, data: Buffer.alloc(4) }])), /required patch/);
  assert.equal(packAssets(wad([{ name: 'UNKNOWN', data: Buffer.alloc(4) }])).metadata.cooked_patches, 0);
  assert.equal(packAssets(wad([{ name: 'UNKNOWN', data: patch() }])).metadata.cooked_patches, 1);
});

test('last duplicate name resolves PNAMES without changing directory indexing', () => {
  const source = wad([{ name: 'PATCH', data: Buffer.alloc(2) }, { name: 'PATCH', data: patch() },
    { name: 'PNAMES', data: pnames(['PATCH']) }, { name: 'TEXTURE1', data: texture(0) }]);
  const { image, metadata } = packAssets(source);
  assert.equal(metadata.lump_count, 4);
  assert.equal(image.readBigUInt64LE(metadata.index_word * 2), 0n);
  assert.ok(image.readUInt32LE(metadata.index_word * 2 + 8) > 0);
  assert.equal(image.readUInt16LE(metadata.pnames_word * 2), 1);
  assert.equal(metadata.pnames_count, 1);
  assert.equal(metadata.pnames_lump, 2);
});

test('decoded directory preserves raw names and serializes only relocatable pointer slots', () => {
  const rawName = Buffer.from([97, 0, 255, 98, 0, 17, 128, 90]);
  const source = wad([{ name: rawName, data: Buffer.from([7, 8, 9]) },
    { name: 'other', data: Buffer.alloc(0) }], { gap: 65537 });
  const { image, metadata } = packAssets(source);
  const original = wadDirectory(source);
  for (let i = 0; i < original.length; ++i) {
    const at = (metadata.directory_word + i * 18) * 2;
    assert.deepEqual(Array.from({ length: 8 }, (_, j) => image.readUInt16LE(at + j * 2)), [...original[i].nameBytes]);
    assert.equal(image.readUInt32LE(at + 16), 0, 'file pointer awaits relocation');
    assert.equal(image.readUInt32LE(at + 20), original[i].offset);
    assert.equal(image.readUInt32LE(at + 24), original[i].length);
    assert.equal(image.readUInt32LE(at + 28), 0, 'cache pointer starts null');
    assert.equal(image.readUInt16LE(at + 34), 0, 'next high word starts zero');
    const next = image.readUInt16LE(at + 32);
    assert.ok(next === 0xffff || next < i, 'chain points only to an earlier record');
  }
});

test('16-bit hash chains preserve collisions, last duplicates and ASCII-only name folding', () => {
  // BigInt arithmetic supplies a separate fixed-width reference calculation.
  function folded(bytes) {
    const end = bytes.indexOf(0), raw = end < 0 ? bytes : bytes.subarray(0, end);
    return Buffer.from([...raw].map(c => c >= 97 && c <= 122 ? c - 32 : c));
  }
  function referenceHash(bytes) {
    let h = 5381n;
    for (const c of folded(bytes)) h = BigInt.asUintN(16, (h << 5n) ^ h ^ BigInt(c));
    return Number(h);
  }
  const names = [Buffer.from('abcdefgh'), Buffer.from('ABCDEFGH'), Buffer.from([0xe0]),
    Buffer.from([0xc0]), Buffer.from([0xdf]), Buffer.from('SS'), Buffer.from('ab\0tail'), Buffer.from('AB'), Buffer.from('XY')];
  const source = wad(names.map(name => ({ name, data: Buffer.alloc(0) })));
  const { image, metadata } = packAssets(source), entries = wadDirectory(source);
  assert.notEqual(entries[2].name, entries[3].name, 'high octets do not fold like Unicode letters');
  assert.notEqual(entries[4].name, entries[5].name, 'one high octet never expands to SS');
  const expectedHeads = Array(entries.length).fill(0xffff), expectedNext = [];
  for (const entry of entries) {
    const bucket = referenceHash(entry.nameBytes) % entries.length;
    expectedNext.push(expectedHeads[bucket]); expectedHeads[bucket] = entry.index;
  }
  assert.ok(expectedNext.some(index => index !== 0xffff), 'fixture exercises collisions');
  for (let i = 0; i < entries.length; ++i) {
    assert.equal(image.readUInt16LE((metadata.hash_heads_word + i) * 2), expectedHeads[i]);
    assert.equal(image.readUInt16LE((metadata.directory_word + i * 18 + 16) * 2), expectedNext[i]);
  }
  function lookup(raw) {
    const bucket = referenceHash(raw) % entries.length;
    let i = image.readUInt16LE((metadata.hash_heads_word + bucket) * 2);
    while (i !== 0xffff) {
      if (folded(entries[i].nameBytes).equals(folded(raw))) return i;
      i = image.readUInt16LE((metadata.directory_word + i * 18 + 16) * 2);
    }
    return -1;
  }
  assert.equal(lookup(Buffer.from('AbCdEfGh')), 1);
  assert.equal(lookup(Buffer.from('ab')), 7);
  assert.equal(lookup(Buffer.from([0xe0])), 2);
  assert.equal(lookup(Buffer.from([0xc0])), 3);
  assert.equal(lookup(Buffer.from('MISSING')), -1);
});

test('PNAMES metadata chooses final table and final matching lump, retaining unresolved names', () => {
  const source = wad([{ name: 'PNAMES', data: pnames(['STALE']) },
    { name: 'Patch', data: Buffer.alloc(1) }, { name: 'PATCH', data: patch() },
    { name: Buffer.from([0xe0]), data: Buffer.alloc(0) },
    { name: Buffer.from([0xc0]), data: Buffer.alloc(0) },
    { name: 'pnames', data: pnames(['patch', 'UNUSED', Buffer.from([0xe0]), Buffer.from([0xc0])]) },
    { name: 'TEXTURE1', data: texture(0) }]);
  const { image, metadata } = packAssets(source);
  assert.equal(metadata.pnames_lump, 5);
  assert.equal(metadata.pnames_count, 4);
  assert.deepEqual(Array.from({ length: 4 }, (_, i) => image.readUInt16LE((metadata.pnames_word + i) * 2)),
    [2, 0xffff, 3, 4]);
  assert.equal(metadata.texture_word, metadata.pnames_word + 4);
  assert.equal(image.readUInt32LE((metadata.index_word + 2 * 4) * 2), metadata.renderer_word + metadata.renderer_words);
});

test('texture metadata preserves definitions, clips negative origins and uses cooked column offsets', () => {
  const source = wad([{ name: 'PNAMES', data: pnames(['LEFT', 'RIGHT']) },
    { name: 'TEXTURE1', data: textureTable([{ name: 'WALL', width: 5, height: 3,
      patches: [{ x: -1, y: -7 }, { index: 1, x: 3, y: 19 }] }]) },
    { name: 'LEFT', data: columnPatch(4) }, { name: 'RIGHT', data: columnPatch(2) }]);
  const packed = packAssets(source), { metadata, image } = packed, decoded = decodedTextures(packed);
  assert.equal(metadata.texture_word, metadata.pnames_word + metadata.pnames_count);
  assert.equal(image.readUInt32LE(metadata.header_word * 2 + 56), metadata.texture_word);
  assert.equal(image.readUInt32LE(metadata.header_word * 2 + 60), metadata.texture_words);
  assert.equal(metadata.texture_count, 1); assert.equal(metadata.texture_columns, 5);
  assert.equal(metadata.texture_definition_words, 20);
  assert.deepEqual(decoded.sourceIds, [1, 0xffff, 0]);
  assert.deepEqual(decoded.definitions, [{ name: Buffer.from('WALL\0\0\0\0'), width: 5, height: 3,
    index: 0, next: 0xffff, nextHigh: 0, parts: [{ x: -1, y: -7, lump: 2 }, { x: 3, y: 19, lump: 3 }] }]);
  assert.deepEqual(decoded.lumps, [2, 2, 2, 3, 3]);
  assert.deepEqual(decoded.offsets, [29, 35, 41, 23, 29]);
  assert.deepEqual(decoded.masks, [3]); assert.deepEqual(decoded.heights, [3 * 65536]);
  assert.deepEqual(decoded.sizes, [0]); assert.deepEqual(decoded.heads, [0]);
});

test('overlap lookup keeps patch order, pads short columns and bounds composite allocation', () => {
  function cooked(width, height, count = 2) {
    return packAssets(wad([{ name: 'PNAMES', data: pnames(['PATCH']) },
      { name: 'TEXTURE1', data: textureTable([{ name: 'OVERLAP', width, height,
        patches: Array.from({ length: count }, (_, i) => ({ y: -i })) }]) },
      { name: 'PATCH', data: columnPatch(width, height) }]));
  }
  for (const [height, pitch] of [[3, 128], [128, 128], [256, 256]]) {
    const result = decodedTextures(cooked(3, height, 3));
    assert.deepEqual(result.lumps, [0xffff, 0xffff, 0xffff]);
    assert.deepEqual(result.offsets, [0, pitch, pitch * 2]);
    assert.deepEqual(result.sizes, [pitch * 3]);
    assert.deepEqual(result.definitions[0].parts.map(p => p.y), [0, -1, -2]);
  }
  assert.deepEqual(decodedTextures(cooked(512, 128)).sizes, [65536]);
  assert.throws(() => cooked(513, 128), /exceeds 64k/);
  assert.deepEqual(decodedTextures(cooked(2, 32767)).sizes, [65534]);
  assert.throws(() => cooked(3, 32767), /exceeds 64k/);
  const mixed = decodedTextures(packAssets(wad([{ name: 'PNAMES', data: pnames(['WIDE', 'NARROW']) },
    { name: 'TEXTURE1', data: textureTable([{ name: 'MIXED', width: 4, height: 16,
      patches: [{}, { index: 1, x: 1 }] }]) }, { name: 'WIDE', data: columnPatch(4) },
    { name: 'NARROW', data: columnPatch(2) }])));
  assert.deepEqual(mixed.lumps, [2, 0xffff, 0xffff, 2]);
  assert.deepEqual(mixed.offsets, [23, 0, 128, 41]);
  assert.deepEqual(mixed.sizes, [256]);
});

test('an empty final patch column retains its valid marker-relative pixel offset', () => {
  const empty = Buffer.alloc(13);
  empty.writeUInt16LE(1); empty.writeUInt16LE(1, 2); empty.writeUInt32LE(12, 8); empty[12] = 255;
  const packed = packAssets(wad([{ name: 'PNAMES', data: pnames(['EMPTY']) },
    { name: 'TEXTURE1', data: textureTable([{ name: 'EMPTYTEX', width: 1, height: 1, patches: [{}] }]) },
    { name: 'EMPTY', data: empty }]));
  const decoded = decodedTextures(packed), cooked = cookPatch(empty);
  assert.deepEqual(decoded.lumps, [2]); assert.deepEqual(decoded.offsets, [23]);
  assert.equal(decoded.offsets[0], cooked.length / 2 + 2);
  assert.equal(cooked.readUInt16LE((decoded.offsets[0] - 3) * 2), 255);
});

test('texture sources use final WAD names, combine TEXTURE2, and first texture duplicate wins', () => {
  const definition = name => ({ name, width: 1, height: 3, patches: [{}] });
  const source = wad([{ name: 'PNAMES', data: pnames(['PATCH']) },
    { name: 'TEXTURE1', data: Buffer.alloc(4) },
    { name: 'TEXTURE1', data: textureTable([definition('DUP'), definition('OTHER')]) },
    { name: 'TEXTURE2', data: textureTable([definition('dup'), definition('last')]) },
    { name: 'PATCH', data: patch() }]);
  const decoded = decodedTextures(packAssets(source));
  assert.deepEqual(decoded.sourceIds, [2, 3, 0]);
  assert.deepEqual(decoded.definitions.map(d => d.index), [0, 1, 2, 3]);
  const seen = new Set();
  for (const head of decoded.heads) {
    let index = head, last = -1;
    while (index !== 0xffff) {
      assert.ok(index > last && index < 4, 'hash chains only move forwards');
      assert.ok(!seen.has(index), 'each texture appears once'); seen.add(index);
      const definition = decoded.definitions[index];
      assert.equal(definition.nextHigh, 0); last = index; index = definition.next;
    }
  }
  assert.equal(seen.size, 4);
  // Independent byte-wise BigInt hash follows the guest's uint16_t arithmetic.
  function lookup(name) {
    let h = 5381n;
    for (const c of Buffer.from(name.toUpperCase())) h = BigInt.asUintN(16, (h << 5n) ^ h ^ BigInt(c));
    let index = decoded.heads[Number(h % 4n)];
    while (index !== 0xffff) {
      if (decoded.definitions[index].name.toString('latin1').replace(/\0.*$/, '').toUpperCase() === name.toUpperCase()) return index;
      index = decoded.definitions[index].next;
    }
    return -1;
  }
  assert.equal(lookup('dup'), 0); assert.equal(lookup('last'), 3); assert.equal(lookup('absent'), -1);
});

test('texture preparation rejects uncovered columns, invalid dimensions and malformed counts', () => {
  function source(table, includeNames = true) {
    return wad([...(includeNames ? [{ name: 'PNAMES', data: pnames(['PATCH']) }] : []),
      { name: 'TEXTURE1', data: table }, { name: 'PATCH', data: patch() }]);
  }
  const single = textureTable([{ name: 'BAD', width: 1, height: 3, patches: [{}] }]);
  assert.throws(() => packAssets(source(single, false)), /missing PNAMES/);
  for (const [width, height] of [[0, 3], [32768, 3], [1, 0], [1, 32768]])
    assert.throws(() => packAssets(source(textureTable([{ name: 'BAD', width, height, patches: [{}] }]))), /dimensions/);
  assert.throws(() => packAssets(source(textureTable([{ name: 'BAD', width: 1, height: 3, patches: [] }]))), /patch count/);
  assert.throws(() => packAssets(source(textureTable([]))), /no textures/);
  for (const patches of [[{}], [{ x: 2 }], [{ x: -2 }]])
    assert.throws(() => packAssets(source(textureTable([{ name: 'GAP', width: 2, height: 3, patches }]))), /column without a patch/);
  const overlap = Buffer.from(single); overlap.writeUInt32LE(4, 4);
  assert.throws(() => packAssets(source(overlap)), /overlaps offset table/);
  const count = Buffer.alloc(4 + 32768 * 4); count.writeUInt32LE(32768);
  assert.throws(() => packAssets(source(count)), /too many textures/);
});

test('shareware texture metadata contains all definitions and final lookup tables', () => {
  const packed = packAssets(fs.readFileSync(path.join(port, 'assets/doom1.wad')));
  const decoded = decodedTextures(packed);
  assert.equal(packed.metadata.texture_count, 125);
  assert.equal(decoded.definitionWords, 3544); assert.equal(decoded.columns, 10776);
  assert.equal(decoded.definitions.reduce((sum, item) => sum + item.parts.length, 0), 598);
  assert.equal(packed.metadata.texture_words, 36638);
  assert.ok(decoded.lumps.includes(0xffff));
  assert.ok(decoded.sizes.some(n => n > 0));
});

test('metadata counts fit signed guest lump IDs and bounded PNAMES storage', () => {
  const many = Array.from({ length: 32768 }, () => ({ name: 'EMPTY', data: Buffer.alloc(0) }));
  assert.throws(() => packAssets(wad(many)), /too many lump IDs/);
  many.pop();
  const maximum = packAssets(wad(many));
  assert.equal(maximum.metadata.lump_count, 32767);
  assert.ok(Array.from({ length: 32767 }, (_, i) => maximum.image.readUInt16LE((maximum.metadata.hash_heads_word + i) * 2)).includes(32766));
  const count = Buffer.alloc(4); count.writeUInt32LE(32768);
  assert.throws(() => packAssets(wad([{ name: 'PNAMES', data: count }])), /too many patch names/);
  assert.throws(() => packAssets(wad([{ name: 'PNAMES', data: Buffer.from([1, 0, 0, 0]) }])), /PNAMES names/);
  const { image, metadata } = packAssets(wad([{ name: 'PNAMES', data: pnames([]) }]));
  assert.equal(metadata.pnames_count, 0);
  assert.equal(metadata.pnames_lump, 0);
  assert.equal(metadata.pnames_word, metadata.renderer_word);
  assert.equal(metadata.renderer_word + metadata.renderer_words, metadata.total_words);
  assert.equal(image.readUInt32LE(metadata.header_word * 2 + 44), 0);
});

test('cache preserves identity, repairs corruption and rejects aliases', t => {
  const directory = temporary(t), source = path.join(directory, 'original.wad'), image = path.join(directory, 'asset.img');
  const original = wad([{ name: 'TITLEPIC', data: patch() }]); fs.writeFileSync(source, original);
  const first = cachedAssets(source, image), mtime = fs.statSync(image).mtimeMs;
  assert.deepEqual(cachedAssets(source, image), first);
  assert.equal(fs.statSync(image).mtimeMs, mtime);
  fs.writeFileSync(image, Buffer.alloc(3));
  assert.deepEqual(cachedAssets(source, image), first);
  assert.equal(sha(fs.readFileSync(image)), first.sha256);
  assert.throws(() => cachedAssets(source, source), /must differ/);
  const hardlink = path.join(directory, 'hardlink'); fs.linkSync(source, hardlink);
  assert.throws(() => cachedAssets(source, hardlink), /hard link/);
  const symlink = path.join(directory, 'symlink'); fs.symlinkSync(source, symlink);
  assert.throws(() => cachedAssets(source, symlink), /must differ/);
  assert.deepEqual(fs.readFileSync(source), original);
});

test('config and staging select matching image, preserve source and reject mismatches', t => {
  const directory = temporary(t), source = path.join(directory, 'original.wad');
  const original = wad([{ name: 'TITLEPIC', data: patch() }]); fs.writeFileSync(source, original);
  const generated = path.join(directory, 'generated'), manifest = path.join(directory, 'manifest.json');
  for (const mode of ['packed', 'expanded', 'cooked']) {
    const config = support('config', source, generated, manifest, '64000', 'engine', '0', '0', '', '', 'clang', '', '', '', mode);
    assert.equal(config.status, 0, config.stderr);
    const build = JSON.parse(fs.readFileSync(manifest));
    assert.equal(build.settings.wad_mode, mode);
    assert.equal(build.wad.sha256, sha(original));
    assert.equal(build.asset_image.mode, mode);
    const header = fs.readFileSync(path.join(generated, 'doom_build_config.h'), 'utf8');
    assert.match(header, new RegExp(`#define DOOM_WAD_MODE ${['packed', 'expanded', 'cooked'].indexOf(mode)}\\n`));
    assert.ok(header.includes(`#define DOOM_ASSET_TOTAL_WORDS ${build.asset_image.total_words}UL`));
    for (const [macro, field, suffix] of [
      ['VERSION', 'format_version', 'U'], ['DIRECTORY_WORD', 'directory_word', 'UL'],
      ['HASH_HEADS_WORD', 'hash_heads_word', 'UL'], ['PNAMES_WORD', 'pnames_word', 'UL'],
      ['PNAMES_COUNT', 'pnames_count', 'UL'], ['PNAMES_LUMP', 'pnames_lump', 'UL'],
      ['TEXTURE_WORD', 'texture_word', 'UL'], ['TEXTURE_WORDS', 'texture_words', 'UL'],
      ['TEXTURE_COUNT', 'texture_count', 'UL'], ['TEXTURE_COLUMNS', 'texture_columns', 'UL'],
      ['TEXTURE_DEFINITION_WORDS', 'texture_definition_words', 'UL'],
      ['SPRITE_METRICS_WORD', 'sprite_metrics_word', 'UL'], ['SPRITE_METRICS_WORDS', 'sprite_metrics_words', 'UL'],
      ['SPRITE_WORD', 'sprite_word', 'UL'], ['SPRITE_WORDS', 'sprite_words', 'UL'],
      ['SPRITE_COUNT', 'sprite_count', 'UL'], ['SPRITE_FRAMES', 'sprite_frames', 'UL'],
      ['SPRITE_FIRST_LUMP', 'sprite_first_lump', 'UL'], ['SPRITE_LAST_LUMP', 'sprite_last_lump', 'UL'],
      ['SPRITE_NAMES_LO', 'sprite_names_lo', 'UL'], ['SPRITE_NAMES_HI', 'sprite_names_hi', 'UL'],
      ['RENDERER_WORD', 'renderer_word', 'UL'], ['RENDERER_WORDS', 'renderer_words', 'UL'],
      ['METADATA_ABI', 'metadata_abi', 'U'], ['DIRECTORY_RECORD_WORDS', 'directory_record_words', 'U'],
      ['HASH_ENTRY_WORDS', 'hash_entry_words', 'U'], ['PNAMES_ENTRY_WORDS', 'pnames_entry_words', 'U'],
    ]) assert.ok(header.includes(`#define DOOM_ASSET_${macro} ${build.asset_image[field]}${suffix}\n`));
    const disk = path.join(directory, 'run.img');
    const stage = support('disk', source, disk, mode, manifest);
    assert.equal(stage.status, 0, stage.stderr);
    assert.equal(JSON.parse(stage.stdout).sha256, build.asset_image.sha256);
    assert.deepEqual(fs.readFileSync(disk), fs.readFileSync(build.asset_image.path));
    assert.notEqual(support('disk', source, disk, mode === 'packed' ? 'expanded' : 'packed', manifest).status, 0);
    assert.notEqual(support('disk', source, source, mode, manifest).status, 0);
    assert.notEqual(support('disk', source, build.asset_image.path, mode, manifest).status, 0);
    fs.writeFileSync(build.asset_image.path, Buffer.alloc(1));
    assert.notEqual(support('disk', source, disk, mode, manifest).status, 0);
  }
  assert.deepEqual(fs.readFileSync(source), original);
  const legacy = path.join(directory, 'legacy.img');
  assert.equal(support('disk', source, legacy).status, 0);
  assert.deepEqual(fs.readFileSync(legacy).subarray(0, original.length), original);
});

test('build configuration keys track WAD contents, path, mode and bank size', t => {
  const directory = temporary(t), source = path.join(directory, 'source.wad');
  fs.writeFileSync(source, wad([{ name: 'TITLEPIC', data: patch() }]));
  function key(file = source, mode = 'cooked', bank = '64000', generated = directory) {
    const result = support('config-key', file, mode, bank, generated);
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /^[a-f0-9]{64}\n$/);
    return result.stdout.trim();
  }
  const original = key();
  assert.equal(key(), original);
  assert.notEqual(key(source, 'expanded'), original);
  assert.notEqual(key(source, 'cooked', '32000'), original);
  assert.notEqual(key(source, 'cooked', '64000', path.join(directory, 'other-build')), original);
  const copy = path.join(directory, 'copy.wad'); fs.copyFileSync(source, copy);
  assert.notEqual(key(copy), original);
  const bytes = fs.readFileSync(source); bytes[12 + 15] ^= 1; fs.writeFileSync(source, bytes);
  assert.notEqual(key(), original);
  const stamp = path.join(directory, 'config-stamp');
  assert.equal(support('config-stamp', stamp, original).status, 0);
  const mtime = fs.statSync(stamp).mtimeMs;
  assert.equal(fs.readFileSync(stamp, 'utf8'), `${original}\n`);
  assert.equal(support('config-stamp', stamp, original).status, 0);
  assert.equal(fs.statSync(stamp).mtimeMs, mtime);
  assert.notEqual(support('config-stamp', stamp, 'bad-key').status, 0);
  assert.equal(support('config-invalidate', stamp).status, 0);
  assert.ok(!fs.existsSync(stamp));
  assert.equal(support('config-invalidate', stamp).status, 0);
});

test('incremental builds survive rapid mode and WAD changes with an older shared header', { timeout: 120000 }, t => {
  const root = path.resolve(port, '../..');
  const rvm = path.join(root, 'target/release/rvm');
  const dependencies = [rvm, 'target/release/rcc', 'src/ripple-asm/target/release/rasm',
    'src/ripple-asm/target/release/rlink', 'runtime/crt0.pobj', 'runtime/libruntime.par'];
  if (!dependencies.every(file => fs.existsSync(path.resolve(root, file)))) {
    t.skip('requires the built release toolchain and runtime'); return;
  }
  const directory = temporary(t), source = path.join(port, 'assets/doom1.wad');
  const copiedWad = path.join(directory, 'same-source.wad'); fs.copyFileSync(source, copiedWad);
  const build = path.join(directory, 'build'), generated = path.join(build, 'generated');
  const manifest = path.join(build, 'manifest.json'), disk = path.join(directory, 'run.img');
  function make(mode, wadFile = source, program = 'wad_read_small', goal = '_ripple') {
    const result = spawnSync('make', ['--no-print-directory', '-C', port, '-j2', goal,
      `BUILD=${build}`, `PROGRAM=${program}`, `WAD_MODE=${mode}`, `WAD=${wadFile}`],
    { encoding: 'utf8', timeout: 45000 });
    assert.equal(result.status, 0, `${result.error || ''}\n${result.stdout}\n${result.stderr}`);
    return result.stdout;
  }
  function run(mode, wadFile = source) {
    const staged = support('disk', wadFile, disk, mode, manifest);
    assert.equal(staged.status, 0, staged.stderr);
    const result = spawnSync(rvm, [path.join(build, 'ripple/wad_read_small/wad_read_small.bin'),
      '--headless', '--exit-code', '--bank-size', '64000', '--memory', '16384000', '--disk', disk,
      '--max-instructions', '2000000', '--timeout', '10'], { encoding: 'utf8', timeout: 15000 });
    assert.equal(result.status, 0, `${result.error || ''}\n${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, /WAD READ SMALL OK/);
  }
  make('expanded'); run('expanded');
  for (const [mode, wadFile] of [['packed', source], ['cooked', source], ['expanded', source], ['expanded', copiedWad]]) {
    // Another program can change the common header before this program is
    // revisited. Deliberately make it older than all outputs: content, not
    // timestamp ordering or a one-second sleep, must trigger recompilation.
    make(mode, wadFile, 'wad_read', 'prepare');
    fs.utimesSync(path.join(generated, 'doom_build_config.h'), 1, 1);
    const log = make(mode, wadFile);
    assert.match(log, /Ripple: src\/wad_io\.c/, 'every mode/WAD switch recompiles the shared reader');
    run(mode, wadFile);
  }
  const stable = make('expanded', copiedWad);
  assert.doesNotMatch(stable, /Ripple:/, 'unchanged configuration keeps incremental reuse');
  // Model an interrupted configuration switch: the first translation unit is
  // overwritten, the second fails, and timestamps cannot reveal the partial
  // change. Returning to the old mode must not trust its previous object stamp.
  const objectDirectory = path.join(build, 'ripple/wad_read_small');
  const timestamps = new Map(fs.readdirSync(objectDirectory).map(name => {
    const file = path.join(objectDirectory, name); return [file, fs.statSync(file).mtime];
  }));
  const wrapper = path.join(directory, 'fail-second-compile.sh'), marker = path.join(directory, 'compiled-once');
  const quote = value => `'${value.replaceAll("'", "'\\''")}'`;
  fs.writeFileSync(wrapper, `#!/usr/bin/env bash\nset -eu\nif [[ -e ${quote(marker)} ]]; then exit 1; fi\ntouch ${quote(marker)}\nexec ${quote(path.join(root, 'target/release/rcc'))} "$@"\n`, { mode: 0o755 });
  const failed = spawnSync('make', ['--no-print-directory', '-C', port, '-j1', '_ripple',
    `BUILD=${build}`, 'PROGRAM=wad_read_small', 'WAD_MODE=packed', `WAD=${copiedWad}`, `RCC=${wrapper}`],
  { encoding: 'utf8', timeout: 45000 });
  assert.notEqual(failed.status, 0, 'injected second compilation must fail');
  assert.ok(fs.existsSync(marker), 'the first translation unit was compiled');
  assert.ok(!fs.existsSync(path.join(objectDirectory, '.asset-config')), 'partial objects must remain untrusted');
  for (const [file, time] of timestamps) if (fs.existsSync(file)) fs.utimesSync(file, time, time);
  make('expanded', copiedWad, 'wad_read', 'prepare');
  fs.utimesSync(path.join(generated, 'doom_build_config.h'), 1, 1);
  const recovered = make('expanded', copiedWad);
  assert.match(recovered, /Ripple: tests\/wad_read_small\.c/);
  assert.match(recovered, /Ripple: src\/wad_io\.c/);
  run('expanded', copiedWad);
});
