import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createHash } from 'node:crypto';
import { packAssets, wadDirectory } from './assets-pack.mjs';
import { validateCookedAssets } from './assets-validate.mjs';

function name(target, text, at) {
  if (Buffer.isBuffer(text)) text.copy(target, at, 0, 8);
  else target.write(text, at, 8, 'latin1');
}
function wad(lumps) {
  const directory = 12 + lumps.reduce((sum, lump) => sum + lump.data.length, 0);
  const data = Buffer.alloc(directory + lumps.length * 16);
  data.write('IWAD'); data.writeUInt32LE(lumps.length, 4); data.writeUInt32LE(directory, 8);
  let at = 12;
  lumps.forEach((lump, index) => {
    data.writeUInt32LE(at, directory + index * 16);
    data.writeUInt32LE(lump.data.length, directory + index * 16 + 4);
    name(data, lump.name, directory + index * 16 + 8);
    lump.data.copy(data, at); at += lump.data.length;
  });
  return data;
}
function patch(width = 3, empty = false) {
  const directory = 8 + 4 * width, data = Buffer.alloc(directory + width * 6 + 1);
  data.writeUInt16LE(width); data.writeUInt16LE(16, 2);
  data.writeInt16LE(-2, 4); data.writeInt16LE(-7, 6);
  for (let x = 0; x < width; ++x) {
    data.writeUInt32LE(directory + x * 6, 8 + x * 4);
    data.set([0, 1, 0, x % 251, 0, 255], directory + x * 6);
  }
  data[data.length - 1] = 255;
  if (empty) data.writeUInt32LE(data.length - 1, 8 + (width - 1) * 4);
  return data;
}
function pnames(names) {
  const data = Buffer.alloc(4 + names.length * 8); data.writeUInt32LE(names.length);
  names.forEach((text, i) => name(data, text, 4 + i * 8));
  return data;
}
function textures(definitions) {
  const directory = 4 + definitions.length * 4;
  const data = Buffer.alloc(directory + definitions.reduce((sum, t) => sum + 22 + t.parts.length * 10, 0));
  data.writeUInt32LE(definitions.length);
  let at = directory;
  definitions.forEach((texture, i) => {
    data.writeUInt32LE(at, 4 + i * 4); name(data, texture.name, at);
    data.writeUInt16LE(texture.width, at + 12); data.writeUInt16LE(texture.height, at + 14);
    data.writeUInt16LE(texture.parts.length, at + 20);
    texture.parts.forEach((part, p) => {
      data.writeInt16LE(part.x ?? 0, at + 22 + p * 10);
      data.writeInt16LE(part.y ?? 0, at + 24 + p * 10);
      data.writeUInt16LE(part.patch ?? 0, at + 26 + p * 10);
    });
    at += 22 + texture.parts.length * 10;
  });
  return data;
}
function fixture() {
  const source = wad([{ name: 'PNAMES', data: pnames(['PATCH', 'OVERLAY', 'UNUSED']) },
    { name: 'TEXTURE1', data: textures([
      { name: 'Dup', width: 3, height: 16, parts: [{}, { patch: 1, x: 1, y: -1 }] },
      { name: Buffer.from([0xe0, 97, 0, 253, 17, 0, 0, 0]), width: 1, height: 32767, parts: [{ x: -1, y: -32768 }] },
    ]) }, { name: 'TEXTURE2', data: textures([{ name: 'dup', width: 3, height: 128, parts: [{}] }]) },
    { name: 'PATCH', data: Buffer.alloc(0) }, { name: 'PATCH', data: patch(3, true) },
    { name: 'OVERLAY', data: patch(2) }, { name: 'RAW', data: Buffer.from([0, 128, 255]) }]);
  return { source, ...packAssets(source) };
}
function rejected(image, metadata, pattern = /Invalid cooked assets:/) {
  assert.throws(() => validateCookedAssets(image, metadata), error => {
    assert.ok(error instanceof Error);
    assert.match(error.message, /^Invalid cooked assets:/, 'malformed data must not escape as a native Buffer exception');
    assert.match(error.message, pattern);
    return true;
  });
}
function change(fixture, modifications, pattern) {
  const image = Buffer.from(fixture.image);
  for (const [word, value] of modifications) image.writeUInt16LE(value, word * 2);
  assert.ok(!image.equals(fixture.image), 'mutation must change a cell');
  rejected(image, fixture.metadata, pattern);
}
function resignalSource(image, originalMetadata) {
  const metadata = { ...originalMetadata }, source = Buffer.alloc(metadata.source_octets);
  for (let i = 0; i < source.length; ++i) source[i] = image.readUInt16LE(i * 2);
  metadata.source_sha256 = createHash('sha256').update(source).digest('hex');
  const fingerprint = Buffer.from(metadata.source_sha256, 'hex').readUInt32LE(0);
  metadata.fingerprint_lo = fingerprint & 65535; metadata.fingerprint_hi = fingerprint >>> 16;
  image.writeUInt32LE(fingerprint, (metadata.header_word + 12) * 2);
  return metadata;
}

test('readback accepts complete metadata without modifying the image or manifest', () => {
  const f = fixture(), before = Buffer.from(f.image), metadata = Object.freeze({ ...f.metadata });
  assert.deepEqual(validateCookedAssets(f.image, metadata), {
    lumps: 7, patches: 2, textures: 3, texture_columns: 7, sprites: 0, sprite_frames: 0, renderer_views: 18, total_words: metadata.total_words,
  });
  assert.deepEqual(f.image, before); assert.deepEqual(metadata, f.metadata);
  // The final direct column is a legal marker-relative offset length+2.
  const h = metadata.texture_word, defs = h + 16 + metadata.texture_definition_words;
  const offsets = defs + metadata.texture_columns;
  const lastOffset = f.image.readUInt32LE((offsets + 6 * 2) * 2);
  const patchLength = f.image.readUInt32LE((metadata.index_word + 4 * 4 + 2) * 2);
  assert.equal(lastOffset, patchLength + 2);
});

test('minimal images allow absent textures, absent PNAMES and unresolved unused names', () => {
  for (const lumps of [[{ name: 'RAW', data: Buffer.alloc(0) }],
    [{ name: 'TITLEPIC', data: patch(1) }],
    [{ name: 'PNAMES', data: pnames(['MISSING']) }, { name: 'RAW', data: Buffer.alloc(0) }]]) {
    const { image, metadata } = packAssets(wad(lumps));
    const summary = validateCookedAssets(image, metadata);
    assert.equal(summary.textures, 0); assert.equal(summary.texture_columns, 0);
  }
});

test('all outer and texture header words are independently checked', () => {
  const f = fixture();
  for (const [start, count] of [[f.metadata.header_word, 44], [f.metadata.texture_word, 16]])
    for (let i = 0; i < count; ++i) {
      const at = start + i;
      change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /header/);
    }
});

test('directory links, pointers, bounds, hashes and PNAMES IDs match the original WAD', () => {
  const f = fixture(), m = f.metadata;
  for (let i = 0; i < 18; ++i) {
    const at = m.directory_word + i;
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /directory/);
  }
  for (const [at, label] of [[m.hash_heads_word, /directory hash head/], [m.pnames_word, /PNAMES patch ID/]])
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], label);
  // A different but in-range lump ID must fail too, not just an unsafe value.
  change(f, [[m.pnames_word, 5]], /PNAMES patch ID/);
});

test('patch indices and every kind of serialized patch data are checked', () => {
  const f = fixture(), index = f.metadata.index_word + 4 * 4;
  const start = f.image.readUInt32LE(index * 2), length = f.image.readUInt32LE((index + 2) * 2);
  for (let i = 0; i < 4; ++i) {
    const at = index + i;
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /patch/);
  }
  for (const offset of [0, 1, 2, 3, 4, 5, 10, 11, 20, length - 1]) {
    const at = start + offset;
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /patch/);
  }
  change(f, [[f.metadata.index_word, start & 65535], [f.metadata.index_word + 1, start >>> 16]], /patch/);
});

test('texture definitions, signed origins, references and hash links match source records', () => {
  const f = fixture(), start = f.metadata.texture_word + 16;
  for (let i = 0; i < 20; ++i) {
    const at = start + i;
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /texture/);
  }
  change(f, [[start + 11, 0]], /texture hash link/);
  change(f, [[start + 16, 5]], /texture patch reference/);
});

test('valid-range column, scalar and hash corruption is rejected semantically', () => {
  const f = fixture(), m = f.metadata;
  const lumps = m.texture_word + 16 + m.texture_definition_words, offsets = lumps + m.texture_columns;
  const masks = offsets + m.texture_columns * 2, heights = masks + m.texture_count;
  const sizes = heights + m.texture_count * 2, heads = sizes + m.texture_count * 2;
  change(f, [[lumps, 5]], /texture column lump/);
  change(f, [[lumps + 1, 4]], /texture column lump/);
  change(f, [[offsets, f.image.readUInt16LE(offsets * 2) + 1]], /texture column offset/);
  change(f, [[offsets + 2, 1]], /texture column offset/);
  change(f, [[offsets + 1, 65535]], /texture column offset/);
  for (const [at, label] of [[masks, /width mask/], [heights, /fixed height/],
    [sizes, /composite size/], [heads, /hash head/]])
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], label);
});

test('source identity, prefix octets, image padding and finite metadata are mandatory', () => {
  const f = fixture();
  change(f, [[12, 256]], /non-octet/);
  change(f, [[12, (f.image.readUInt16LE(24) + 1) & 255]], /source SHA-256/);
  const padding = Buffer.from(f.image); padding[padding.length - 1] = 1;
  rejected(padding, f.metadata, /padding/);
  rejected(f.image.subarray(0, -1), f.metadata, /image length/);
  for (const value of [null, undefined, {}, [], { ...f.metadata, mode: 'expanded' }]) rejected(f.image, value);
  for (const field of ['total_words', 'source_octets', 'header_word', 'lump_count', 'texture_count',
    'texture_columns', 'texture_definition_words', 'pnames_lump', 'fingerprint_hi',
    'sprite_word', 'sprite_words', 'sprite_count', 'sprite_frames', 'sprite_first_lump',
    'sprite_last_lump', 'sprite_names_lo', 'sprite_names_hi', 'renderer_word', 'renderer_words', 'sprite_metrics_word', 'sprite_metrics_words'])
    for (const value of [-1, 1.5, Infinity, NaN, Number.MAX_SAFE_INTEGER])
      rejected(f.image, { ...f.metadata, [field]: value });
  rejected('not a buffer', f.metadata);
});

test('malformed source fields with a recomputed identity still fail with bounded diagnostics', () => {
  const f = fixture(), directory = f.source.readUInt32LE(8), entries = wadDirectory(f.source);
  for (const [octet, bytes] of [[8, [255, 255, 255, 255]],
    [directory + 4, [255, 255, 255, 255]], [entries[0].offset, [255, 255, 255, 255]],
    [entries[1].offset + 4, [255, 255, 255, 255]], [entries[4].offset + 8, [255, 255, 255, 255]]]) {
    const image = Buffer.from(f.image);
    bytes.forEach((value, i) => image.writeUInt16LE(value, (octet + i) * 2));
    rejected(image, resignalSource(image, f.metadata));
  }
});

test('large column sections and exact 64k composites survive readback', () => {
  const definitions = Array.from({ length: 80 }, (_, i) => ({ name: 'TEX' + (i % 13),
    width: 1000, height: i % 2 ? 16 : 256, parts: [{ x: -8 }, { patch: 1, x: i % 61, y: -2 }] }));
  const source = wad([{ name: 'PNAMES', data: pnames(['BASE', 'OVERLAY']) },
    { name: 'TEXTURE1', data: textures(definitions.slice(0, 40)) },
    { name: 'TEXTURE2', data: textures(definitions.slice(40)) },
    { name: 'BASE', data: patch(1024, true) }, { name: 'OVERLAY', data: patch(64) }]);
  const f = packAssets(source);
  assert.equal(validateCookedAssets(f.image, f.metadata).texture_columns, 80000);
  const maximum = packAssets(wad([{ name: 'PNAMES', data: pnames(['BASE']) },
    { name: 'TEXTURE1', data: textures([{ name: 'MAX', width: 512, height: 128, parts: [{}, {}] }]) },
    { name: 'BASE', data: patch(512) }]));
  assert.equal(validateCookedAssets(maximum.image, maximum.metadata).textures, 1);
});

test('shareware image readback validates every prepared patch and texture', () => {
  const source = fs.readFileSync(new URL('../assets/doom1.wad', import.meta.url));
  const { image, metadata } = packAssets(source);
  assert.deepEqual(validateCookedAssets(image, metadata), {
    lumps: 1264, patches: 968, textures: 125, texture_columns: 10776, sprites: 138, sprite_frames: 261, renderer_views: 18, total_words: metadata.total_words,
  });
});

test('sprite and renderer sections are included in the complete artifact validation', () => {
  const source = fs.readFileSync(new URL('../assets/doom1.wad', import.meta.url));
  const f = packAssets(source), m = f.metadata;
  assert.equal(m.sprite_word, m.texture_word + m.texture_words);
  assert.equal(m.sprite_metrics_word, m.sprite_word + m.sprite_words);
  assert.equal(m.renderer_word, m.sprite_metrics_word + m.sprite_metrics_words);
  assert.equal(m.sprite_metrics_words, 16 + 483 * 6);
  for (const offset of [0, 2, 4, 6, 15, 16, 17, 16 + 483 * 2, 16 + 483 * 4 + 1]) {
    const at = m.sprite_metrics_word + offset;
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /sprite metrics/i);
  }
  assert.ok(m.renderer_words > 64000, 'renderer section crosses storage blocks');
  const frames = m.sprite_word + 16 + m.sprite_count * 3;
  for (const at of [m.sprite_word, m.sprite_word + 16, frames, frames + 1, frames + 9])
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /sprite/i);
  const view = m.renderer_word + 16;
  const angles = m.renderer_word + f.image.readUInt32LE((view + 6) * 2);
  for (const at of [m.renderer_word, view, angles, m.renderer_word + 304])
    change(f, [[at, (f.image.readUInt16LE(at * 2) + 1) & 65535]], /renderer/i);
  for (const field of ['sprite_count', 'sprite_frames', 'sprite_names_lo', 'renderer_words'])
    rejected(f.image, { ...m, [field]: m[field] + 1 });
});
