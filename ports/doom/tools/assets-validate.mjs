// Validate the serialized cooked image before a fast guest consumes it. This
// runs when packing assets, not in either VM or on every launch. Read the image against its
// embedded original WAD; do not trust the packer's transient parsed objects.
import { createHash } from 'node:crypto';
import { validateSpriteMetadata } from './sprite-metadata.mjs';
import { validateSpriteMetrics } from './sprite-metrics.mjs';
import { validateRendererTables } from './renderer-tables-validate.mjs';

const fail = message => { throw new Error('Invalid cooked assets: ' + message); };
const check = (condition, message) => { if (!condition) fail(message); };
const integer = (value, maximum, label) => check(Number.isSafeInteger(value)
  && value >= 0 && value <= maximum, label + ' is outside its integer range');
function span(length, offset, count, label) {
  integer(offset, length, label + ' offset');
  integer(count, length - offset, label + ' length');
}
function asciiName(bytes) {
  let result = '';
  for (let i = 0; i < 8 && bytes[i] !== 0; ++i) {
    const c = bytes[i];
    result += String.fromCharCode(c >= 97 && c <= 122 ? c - 32 : c);
  }
  return result;
}
function nameHash(name) {
  let hash = 5381;
  for (const c of name) hash = ((hash * 32) ^ hash ^ c.charCodeAt(0)) & 65535;
  return hash;
}
function rawReaders(bytes, label) {
  return {
    u16(at) { span(bytes.length, at, 2, label); return bytes.readUInt16LE(at); },
    u32(at) { span(bytes.length, at, 4, label); return bytes.readUInt32LE(at); },
  };
}

export function validateCookedAssets(image, metadata) {
  check(Buffer.isBuffer(image), 'image must be a Buffer');
  check(metadata && typeof metadata === 'object', 'metadata is missing');
  check(metadata.mode === 'cooked' && metadata.format_version === 5, 'expected cooked format version 5');
  integer(metadata.total_words, 0x80000000, 'total words');
  integer(metadata.source_octets, metadata.total_words, 'source octets');
  check(metadata.source_octets >= 12, 'source WAD header is truncated');
  check(image.length === Math.ceil(metadata.total_words * 2 / 65536) * 65536,
    'image length does not match padded word count');
  check(metadata.octets === image.length && metadata.padding_octets === image.length - metadata.total_words * 2,
    'image length or padding metadata differs');
  check(image.subarray(metadata.total_words * 2).every(octet => octet === 0), 'nonzero image padding');
  for (const field of ['header_word', 'index_word', 'directory_word', 'hash_heads_word', 'pnames_word',
    'texture_word', 'texture_words', 'texture_columns', 'texture_definition_words',
    'sprite_word', 'sprite_words', 'sprite_frames', 'renderer_word', 'renderer_words',
    'sprite_metrics_word', 'sprite_metrics_words'])
    integer(metadata[field], metadata.total_words, field);
  for (const field of ['lump_count', 'pnames_count', 'texture_count', 'sprite_count']) integer(metadata[field], 32767, field);
  for (const field of ['sprite_first_lump', 'sprite_last_lump', 'sprite_names_lo', 'sprite_names_hi'])
    integer(metadata[field], 65535, field);
  check(metadata.lump_count > 0, 'empty lump directory');
  integer(metadata.cooked_patches, metadata.lump_count, 'cooked patches');
  integer(metadata.pnames_lump, 0xffffffff, 'PNAMES lump');
  integer(metadata.fingerprint_lo, 65535, 'fingerprint low word');
  integer(metadata.fingerprint_hi, 65535, 'fingerprint high word');
  check(metadata.metadata_abi === 1 && metadata.directory_record_words === 18
    && metadata.hash_entry_words === 1 && metadata.pnames_entry_words === 1, 'unsupported metadata ABI');
  check(typeof metadata.source_sha256 === 'string' && /^[a-f0-9]{64}$/.test(metadata.source_sha256),
    'invalid source SHA-256');
  const fingerprint = Buffer.from(metadata.source_sha256, 'hex').readUInt32LE(0);
  check(metadata.fingerprint_lo === (fingerprint & 65535) && metadata.fingerprint_hi === fingerprint >>> 16,
    'fingerprint differs from source SHA-256');
  const words = metadata.total_words;
  const u16 = (word, label = 'word') => { span(words, word, 1, label); return image.readUInt16LE(word * 2); };
  const u32 = (word, label = 'word pair') => { span(words, word, 2, label); return image.readUInt32LE(word * 2); };
  check(metadata.header_word === metadata.source_octets && metadata.index_word === metadata.header_word + 44
    && metadata.directory_word === metadata.index_word + metadata.lump_count * 4
    && metadata.hash_heads_word === metadata.directory_word + metadata.lump_count * 18
    && metadata.pnames_word === metadata.hash_heads_word + metadata.lump_count,
  'noncanonical metadata section layout');
  span(words, metadata.header_word, 44, 'outer header');
  span(words, metadata.pnames_word, metadata.pnames_count, 'PNAMES section');
  let patchStart = metadata.pnames_word + metadata.pnames_count;
  if (metadata.texture_words === 0) {
    check(metadata.texture_word === 0 && metadata.texture_count === 0 && metadata.texture_columns === 0
      && metadata.texture_definition_words === 0, 'absent texture section has nonzero metadata');
  } else {
    check(metadata.texture_word === patchStart && metadata.texture_count > 0 && metadata.texture_words >= 16,
      'invalid texture section layout');
    span(words, metadata.texture_word, metadata.texture_words, 'texture section');
    patchStart += metadata.texture_words;
  }
  if (metadata.sprite_words === 0) {
    check(['sprite_word', 'sprite_count', 'sprite_frames', 'sprite_first_lump', 'sprite_last_lump',
      'sprite_names_lo', 'sprite_names_hi'].every(field => metadata[field] === 0),
    'absent sprite section has nonzero metadata');
  } else {
    check(metadata.sprite_word === patchStart && metadata.sprite_words >= 16, 'invalid sprite section layout');
    span(words, metadata.sprite_word, metadata.sprite_words, 'sprite section');
    patchStart += metadata.sprite_words;
  }
  if (metadata.sprite_metrics_words === 0) {
    check(metadata.sprite_metrics_word === 0, 'absent sprite metrics have nonzero offset');
  } else {
    check(metadata.sprite_metrics_word === patchStart && metadata.sprite_metrics_words >= 16,
      'invalid sprite metrics section layout');
    span(words, metadata.sprite_metrics_word, metadata.sprite_metrics_words, 'sprite metrics section');
    patchStart += metadata.sprite_metrics_words;
  }
  check(metadata.renderer_word === patchStart && metadata.renderer_words >= 16, 'invalid renderer section layout');
  span(words, metadata.renderer_word, metadata.renderer_words, 'renderer section');
  patchStart += metadata.renderer_words;
  const h = metadata.header_word;
  for (const [offset, value] of [[0, 0x5257], [1, 0x4144], [2, 5], [3, 1],
    [24, 1], [25, 18], [26, 1], [27, 1]])
    check(u16(h + offset) === value, 'outer header word ' + offset + ' differs');
  for (const [offset, value] of [[4, metadata.source_octets], [6, metadata.lump_count],
    [8, metadata.index_word], [10, words], [12, fingerprint], [14, metadata.directory_word],
    [16, metadata.hash_heads_word], [18, metadata.pnames_word], [20, metadata.pnames_count],
    [22, metadata.pnames_lump], [28, metadata.texture_word], [30, metadata.texture_words],
    [32, metadata.sprite_word], [34, metadata.sprite_words],
    [36, metadata.renderer_word], [38, metadata.renderer_words],
    [40, metadata.sprite_metrics_word], [42, metadata.sprite_metrics_words]])
    check(u32(h + offset) === value, 'outer header pair ' + offset + ' differs');

  const source = Buffer.alloc(metadata.source_octets);
  for (let i = 0; i < source.length; ++i) {
    const cell = image.readUInt16LE(i * 2);
    check(cell <= 255, 'expanded source contains a non-octet cell at ' + i);
    source[i] = cell;
  }
  check(createHash('sha256').update(source).digest('hex') === metadata.source_sha256,
    'embedded source SHA-256 differs');
  check(['IWAD', 'PWAD'].includes(source.toString('latin1', 0, 4)), 'invalid source WAD signature');
  check(source.readUInt32LE(4) === metadata.lump_count, 'source lump count differs');
  const sourceDirectory = source.readUInt32LE(8);
  span(source.length, sourceDirectory, metadata.lump_count * 16, 'source directory');
  const entries = [], byName = new Map(), heads = new Uint16Array(metadata.lump_count).fill(65535);
  for (let i = 0; i < metadata.lump_count; ++i) {
    const at = sourceDirectory + i * 16, record = metadata.directory_word + i * 18;
    const offset = source.readUInt32LE(at), length = source.readUInt32LE(at + 4);
    const nameBytes = source.subarray(at + 8, at + 16), name = asciiName(nameBytes);
    span(source.length, offset, length, 'source lump ' + i);
    for (let n = 0; n < 8; ++n) check(u16(record + n) === nameBytes[n], 'directory name differs at lump ' + i);
    check(u32(record + 8) === 0 && u32(record + 14) === 0 && u16(record + 17) === 0,
      'directory pointer slot is nonzero at lump ' + i);
    check(u32(record + 10) === offset && u32(record + 12) === length, 'directory bounds differ at lump ' + i);
    const bucket = nameHash(name) % metadata.lump_count;
    check(u16(record + 16) === heads[bucket], 'directory hash link differs at lump ' + i);
    heads[bucket] = i;
    entries.push({ index: i, name, nameBytes, offset, length, bytes: source.subarray(offset, offset + length) });
    byName.set(name, i);
  }
  for (let i = 0; i < heads.length; ++i)
    check(u16(metadata.hash_heads_word + i) === heads[i], 'directory hash head differs at bucket ' + i);

  const pnamesId = byName.get('PNAMES') ?? 0xffffffff, patchNames = [];
  check(metadata.pnames_lump === pnamesId, 'PNAMES source ID differs');
  if (pnamesId !== 0xffffffff) {
    const data = entries[pnamesId].bytes, reader = rawReaders(data, 'source PNAMES');
    const count = reader.u32(0);
    check(count === metadata.pnames_count, 'PNAMES count differs');
    span(data.length, 4, count * 8, 'source PNAMES names');
    for (let i = 0; i < count; ++i) patchNames.push(asciiName(data.subarray(4 + i * 8, 12 + i * 8)));
  } else check(metadata.pnames_count === 0, 'missing source PNAMES has entries');
  for (let i = 0; i < patchNames.length; ++i)
    check(u16(metadata.pnames_word + i) === (byName.get(patchNames[i]) ?? 65535), 'PNAMES patch ID differs at ' + i);

  try {
    validateSpriteMetadata(metadata.sprite_words === 0 ? null
      : image.subarray(metadata.sprite_word * 2, (metadata.sprite_word + metadata.sprite_words) * 2), entries);
    validateSpriteMetrics(metadata.sprite_metrics_words === 0 ? null
      : image.subarray(metadata.sprite_metrics_word * 2,
        (metadata.sprite_metrics_word + metadata.sprite_metrics_words) * 2), source, entries);
    validateRendererTables(image.subarray(metadata.renderer_word * 2,
      (metadata.renderer_word + metadata.renderer_words) * 2));
  } catch (error) { fail(error.message); }
  if (metadata.sprite_words !== 0) {
    const s = metadata.sprite_word;
    for (const [offset, field] of [[2, 'sprite_count'], [6, 'sprite_first_lump'], [7, 'sprite_last_lump'],
      [8, 'sprite_names_lo'], [9, 'sprite_names_hi']])
      check(u16(s + offset) === metadata[field], 'sprite metadata differs: ' + field);
    check(u32(s + 4) === metadata.sprite_frames, 'sprite frame count differs');
  }

  const patches = new Map();
  let patchCursor = patchStart;
  for (const entry of entries) {
    const at = metadata.index_word + entry.index * 4, offset = u32(at), length = u32(at + 2);
    if (offset === 0 && length === 0) continue;
    check(offset === patchCursor && length >= 20 && length <= 0x7fffffff,
      'invalid cooked patch index for lump ' + entry.index);
    span(words, offset, length, 'cooked patch ' + entry.index);
    const raw = entry.bytes, reader = rawReaders(raw, 'source patch ' + entry.index);
    const width = reader.u16(0), height = reader.u16(2);
    check(width > 0 && width <= 32767 && height > 0 && height <= 32767, 'invalid source patch dimensions');
    const directoryEnd = 8 + width * 4, headerWords = 4 + 2 * Math.max(width, 8);
    span(raw.length, 0, directoryEnd, 'source patch columns');
    check(length === headerWords + raw.length - directoryEnd, 'cooked patch length differs at lump ' + entry.index);
    for (let i = 0; i < 4; ++i)
      check(u16(offset + i) === reader.u16(i * 2), 'cooked patch header differs at lump ' + entry.index);
    for (let x = 0; x < width; ++x) {
      let post = reader.u32(8 + x * 4);
      check(post >= directoryEnd && post < raw.length, 'invalid source patch column');
      check(u32(offset + 4 + x * 2) === headerWords + post - directoryEnd,
        'cooked patch column offset differs at lump ' + entry.index);
      while (raw[post] !== 255) {
        span(raw.length, post, 4, 'source patch post');
        const count = raw[post + 1];
        check(raw[post] + count <= height, 'source patch post exceeds height');
        span(raw.length, post, count + 5, 'source patch post and marker');
        post += count + 4;
      }
    }
    for (let x = width; x < 8; ++x)
      check(u32(offset + 4 + x * 2) === 0, 'nonzero unused patch column');
    for (let p = directoryEnd; p < raw.length; ++p)
      check(u16(offset + headerWords + p - directoryEnd) === raw[p], 'cooked patch payload differs at lump ' + entry.index);
    patches.set(entry.index, { width, height, raw, headerWords, directoryEnd, length });
    patchCursor += length;
  }
  check(patchCursor === words && patches.size === metadata.cooked_patches,
    'cooked patch payload extent or count differs');

  const textures = [];
  if (byName.has('TEXTURE1')) {
    check(pnamesId !== 0xffffffff, 'textures require PNAMES');
    for (const name of ['TEXTURE1', 'TEXTURE2']) {
      const id = byName.get(name);
      if (id === undefined) continue;
      const data = entries[id].bytes, reader = rawReaders(data, 'source ' + name);
      const count = reader.u32(0), directoryEnd = 4 + count * 4;
      check(count <= 32767 - textures.length, 'too many source textures');
      span(data.length, 0, directoryEnd, 'source texture directory');
      for (let i = 0; i < count; ++i) {
        const at = reader.u32(4 + i * 4);
        check(at >= directoryEnd, 'source texture overlaps directory');
        span(data.length, at, 22, 'source texture definition');
        const width = reader.u16(at + 12), height = reader.u16(at + 14), count = reader.u16(at + 20);
        check(width > 0 && width <= 32767 && height > 0 && height <= 32767 && count > 0 && count <= 32767,
          'invalid source texture definition');
        span(data.length, at + 22, count * 10, 'source texture patches');
        const texture = { name: data.subarray(at, at + 8), width, height, patches: [] };
        for (let p = 0; p < count; ++p) {
          const part = at + 22 + p * 10, index = reader.u16(part + 4);
          check(index < patchNames.length, 'source texture patch index exceeds PNAMES');
          const lump = byName.get(patchNames[index]);
          check(lump !== undefined && patches.has(lump), 'texture patch has no cooked payload');
          texture.patches.push({ originx: data.readInt16LE(part), originy: data.readInt16LE(part + 2), lump });
        }
        textures.push(texture);
      }
    }
    check(textures.length > 0, 'source WAD has no textures');
  }
  check(textures.length === metadata.texture_count, 'texture count differs from source');
  if (textures.length) {
    const count = textures.length, header = metadata.texture_word;
    const definitions = textures.reduce((sum, t) => sum + 14 + t.patches.length * 3, 0);
    const columns = textures.reduce((sum, t) => sum + t.width, 0);
    check(definitions === metadata.texture_definition_words && columns === metadata.texture_columns
      && metadata.texture_words === 16 + definitions + 3 * columns + 6 * count, 'texture section extents differ');
    for (const [offset, expected] of [[0, 0x5458], [1, 1], [2, count], [3, 0],
      [8, byName.get('TEXTURE1')], [9, byName.get('TEXTURE2') ?? 65535], [10, pnamesId]])
      check(u16(header + offset) === expected, 'texture header word ' + offset + ' differs');
    check(u32(header + 4) === definitions && u32(header + 6) === columns, 'texture header extents differ');
    for (let i = 11; i < 16; ++i) check(u16(header + i) === 0, 'texture header padding is nonzero');
    const buckets = Array.from({ length: count }, () => []);
    textures.forEach((texture, i) => buckets[nameHash(asciiName(texture.name)) % count].push(i));
    const next = new Uint16Array(count).fill(65535);
    for (const bucket of buckets) for (let i = 1; i < bucket.length; ++i) next[bucket[i - 1]] = bucket[i];
    const lumpBase = header + 16 + definitions, offsetBase = lumpBase + columns;
    const maskBase = offsetBase + columns * 2, heightBase = maskBase + count;
    const sizeBase = heightBase + count * 2, headBase = sizeBase + count * 2;
    let at = header + 16, columnAt = 0;
    textures.forEach((texture, index) => {
      for (let i = 0; i < 8; ++i) check(u16(at + i) === texture.name[i], 'texture name differs at ' + index);
      check(u16(at + 8) === texture.width && u16(at + 9) === texture.height
        && u16(at + 10) === index && u16(at + 13) === texture.patches.length, 'texture definition differs at ' + index);
      check(u16(at + 11) === next[index] && u16(at + 12) === 0, 'texture hash link differs at ' + index);
      at += 14;
      for (const part of texture.patches) {
        check(u16(at) === (part.originx & 65535) && u16(at + 1) === (part.originy & 65535)
          && u16(at + 2) === part.lump, 'texture patch reference differs at ' + index);
        at += 3;
      }
      let composite = 0;
      const pitch = Math.max(128, texture.height);
      for (let x = 0; x < texture.width; ++x) {
        let coverage = 0, lump, offset;
        for (const part of texture.patches) {
          const patch = patches.get(part.lump), relative = x - part.originx;
          if (relative < 0 || relative >= patch.width) continue;
          ++coverage;
          lump = part.lump;
          offset = patch.headerWords + patch.raw.readUInt32LE(8 + relative * 4) - patch.directoryEnd + 3;
        }
        check(coverage > 0, 'source texture has an uncovered column');
        if (coverage > 1) {
          check(composite <= 65536 - pitch, 'texture composite exceeds 64k');
          lump = 65535; offset = composite; composite += pitch;
        }
        check(u16(lumpBase + columnAt) === lump, 'texture column lump differs at ' + index + ':' + x);
        check(u32(offsetBase + columnAt * 2) === offset, 'texture column offset differs at ' + index + ':' + x);
        ++columnAt;
      }
      check(u16(maskBase + index) === 2 ** Math.floor(Math.log2(texture.width)) - 1,
        'texture width mask differs at ' + index);
      check(u32(heightBase + index * 2) === texture.height * 65536, 'texture fixed height differs at ' + index);
      check(u32(sizeBase + index * 2) === composite, 'texture composite size differs at ' + index);
      check(u16(headBase + index) === (buckets[index][0] ?? 65535), 'texture hash head differs at ' + index);
    });
    check(at === lumpBase && columnAt === columns, 'texture decoder did not consume its sections');
  }
  return { lumps: entries.length, patches: patches.size, textures: textures.length,
    texture_columns: metadata.texture_columns, sprites: metadata.sprite_count,
    sprite_frames: metadata.sprite_frames, renderer_views: 18, total_words: words };
}
