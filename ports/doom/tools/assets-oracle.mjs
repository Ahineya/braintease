// Independent serialized-octet oracle for TITLEPIC, textures and E1M1 geometry.
// It never calls the C decoder or relies on the guest's cell layout.
import { require, hash } from './test-support.mjs';

function span(data, offset, length) {
  require(Number.isSafeInteger(offset) && Number.isSafeInteger(length) && offset >= 0
    && length >= 0 && offset <= data.length && length <= data.length - offset, 'serialized range outside buffer');
  return data.subarray(offset, offset + length);
}
const u16 = (data, offset) => span(data, offset, 2).readUInt16LE();
const u32 = (data, offset) => span(data, offset, 4).readUInt32LE();
function directory(data) {
  require(['IWAD', 'PWAD'].includes(span(data, 0, 4).toString()), 'invalid WAD signature');
  const count = u32(data, 4), start = u32(data, 8);
  require(count > 0, 'empty WAD directory');
  const records = span(data, start, count * 16), entries = [];
  for (let i = 0; i < records.length; i += 16) {
    const offset = u32(records, i), length = u32(records, i + 4);
    span(data, offset, length);
    entries.push({ name: records.subarray(i + 8, i + 16).toString('ascii').replace(/\0+$/, ''), offset, length });
  }
  return entries;
}
function patchImage(patch, width, height) {
  require(u16(patch, 0) === width && u16(patch, 2) === height, 'patch dimensions differ');
  const end = 8 + 4 * width, indexed = Buffer.alloc(width * height);
  span(patch, 0, end);
  for (let x = 0; x < width; x++) {
    let pos = u32(patch, 8 + x * 4);
    require(pos >= end, 'patch column overlaps header');
    for (;;) {
      const top = span(patch, pos, 1)[0];
      if (top === 255) break;
      const run = span(patch, pos + 1, 1)[0];
      require(top + run <= height, 'patch post exceeds height');
      const post = span(patch, pos, run + 4);
      for (let y = 0; y < run; y++) indexed[(top + y) * width + x] = post[3 + y];
      pos += run + 4;
    }
  }
  return indexed;
}

export function inspectAssets(data) {
  const entries = directory(data), names = new Map(entries.map(entry => [entry.name, entry]));
  const lump = name => {
    require(names.has(name), 'missing lump ' + name);
    const entry = names.get(name); return span(data, entry.offset, entry.length);
  };
  const metrics = { wad_length: data.length, wad_lumps: entries.length, malformed_checks: 10 };
  const pnames = lump('PNAMES'), patchNames = u32(pnames, 0);
  span(pnames, 4, patchNames * 8); metrics.pnames = patchNames;
  const textures = lump('TEXTURE1'), textureCount = u32(textures, 0), tableEnd = 4 + textureCount * 4;
  span(textures, 0, tableEnd);
  let references = 0;
  for (let i = 0; i < textureCount; i++) {
    const pos = u32(textures, 4 + i * 4);
    require(pos >= tableEnd, 'texture overlaps offset table'); span(textures, pos, 22);
    require(u16(textures, pos + 12) && u16(textures, pos + 14), 'zero texture dimension');
    const patches = u16(textures, pos + 20); span(textures, pos + 22, patches * 10);
    for (let p = 0; p < patches; p++) {
      require(u16(textures, pos + 26 + p * 10) < patchNames, 'texture patch index outside PNAMES'); references++;
    }
  }
  Object.assign(metrics, { texture1: textureCount, texture_patch_references: references });
  const mapNames = ['THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SEGS', 'SSECTORS', 'NODES', 'SECTORS', 'REJECT', 'BLOCKMAP'];
  const sizes = [10, 14, 30, 4, 12, 4, 28, 26, 1, 2];
  const labels = ['things', 'linedefs', 'sidedefs', 'vertexes', 'segs', 'subsectors', 'nodes', 'sectors', 'reject_octets', 'blockmap_words'];
  const marker = entries.findLastIndex(entry => entry.name === 'E1M1');
  require(marker >= 0 && marker + 10 < entries.length, 'missing or truncated E1M1 namespace');
  const maps = [], counts = [];
  mapNames.forEach((name, i) => {
    const entry = entries[marker + i + 1];
    require(entry.name === name && entry.length % sizes[i] === 0, 'invalid E1M1 lump ' + name);
    maps.push(span(data, entry.offset, entry.length)); counts.push(entry.length / sizes[i]);
    metrics['e1m1_' + labels[i]] = counts[i];
  });
  require([0, 1, 2, 3, 4, 5, 7].every(i => counts[i]), 'empty required map lump');
  require(maps[9].length >= 8, 'truncated BLOCKMAP');
  require(Math.ceil(counts[7] ** 2 / 8) <= maps[8].length, 'truncated REJECT');
  let player;
  for (let pos = 0; pos < maps[0].length; pos += 10) if (u16(maps[0], pos + 6) === 1 && !player)
    player = [maps[0].readInt16LE(pos), maps[0].readInt16LE(pos + 2), u16(maps[0], pos + 4)];
  require(player, 'missing player start');
  ['x', 'y', 'angle'].forEach((label, i) => metrics['e1m1_player_' + label] = player[i]);
  for (let pos = 0; pos < maps[1].length; pos += 14) {
    require(u16(maps[1], pos) < counts[3] && u16(maps[1], pos + 2) < counts[3], 'invalid LINEDEFS vertex');
    for (const side of [u16(maps[1], pos + 10), u16(maps[1], pos + 12)])
      require(side === 65535 || side < counts[2], 'invalid LINEDEFS side');
  }
  for (let pos = 0; pos < maps[2].length; pos += 30)
    require(u16(maps[2], pos + 28) < counts[7], 'invalid SIDEDEFS sector');
  for (let pos = 0; pos < maps[4].length; pos += 12)
    require(u16(maps[4], pos) < counts[3] && u16(maps[4], pos + 2) < counts[3]
      && u16(maps[4], pos + 6) < counts[1] && u16(maps[4], pos + 8) <= 1, 'invalid SEGS reference');
  for (let pos = 0; pos < maps[5].length; pos += 4)
    require(u16(maps[5], pos) + u16(maps[5], pos + 2) <= counts[4], 'invalid SSECTORS range');
  for (let pos = 0; pos < maps[6].length; pos += 28)
    for (const child of [u16(maps[6], pos + 24), u16(maps[6], pos + 26)])
      require(child & 32768 ? (child & 32767) < counts[5] : child < counts[6], 'invalid NODES child');
  const indexed = patchImage(lump('TITLEPIC'), 320, 200), palette = span(lump('PLAYPAL'), 0, 768);
  const raw = Buffer.alloc(indexed.length * 2);
  indexed.forEach((index, i) => raw.writeUInt16LE(((palette[index * 3] >> 3) << 11)
    | ((palette[index * 3 + 1] >> 2) << 5) | (palette[index * 3 + 2] >> 3), i * 2));
  Object.assign(metrics, { validation_ok: 1, title_width: 320, title_height: 200 });
  return { raw, metrics, wad_sha256: hash(data), rgb565_sha256: hash(raw) };
}

export function selfTestAssets(data) {
  let checks = 0;
  function fails(action) {
    let rejected = false;
    try { action(); } catch { rejected = true; }
    require(rejected, 'asset oracle accepted malformed input'); checks++;
  }
  fails(() => directory(data.subarray(0, 11)));
  let bad = Buffer.from(data); bad.writeUInt32LE(0xfffffff0, 8); fails(() => directory(bad));
  bad = Buffer.from(data); bad.writeUInt32LE(0xfffffffe, u32(data, 8)); bad.writeUInt32LE(4, u32(data, 8) + 4);
  fails(() => directory(bad));
  const patch = Buffer.from([1, 0, 1, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 1, 0, 7, 0, 255]);
  require(patchImage(patch, 1, 1).equals(Buffer.from([7])), 'valid patch failed'); checks++;
  fails(() => patchImage(patch.subarray(0, -1), 1, 1));
  for (const [index, value] of [[8, 8], [12, 1], [13, 255]]) {
    const malformed = Buffer.from(patch); malformed[index] = value;
    fails(() => patchImage(malformed, 1, 1));
  }
  fails(() => patchImage(patch, 2, 1));
  return checks;
}

export function checkAssetLog(log, metrics) {
  const actual = Object.fromEntries([...log.matchAll(/^ASSET (\w+) (-?\d+)\s*$/gm)].map(match => [match[1], Number(match[2])]));
  for (const [key, value] of Object.entries(metrics))
    require(actual[key] === value, 'asset metric ' + key + ': expected ' + value + ', got ' + actual[key]);
}
