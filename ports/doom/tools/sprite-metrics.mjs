// Final 16.16 sprite dimensions/origins. Unlike sprite definitions, every lump
// in the sprite namespace contributes, including names the engine never uses.
export const SPRITE_METRICS_HEADER_WORDS = 16;
const demand = (condition, message) => { if (!condition) throw new Error('Invalid sprite metrics: ' + message); };

function sourceRange(source, entries) {
  demand(Buffer.isBuffer(source) && Array.isArray(entries) && entries.length <= 32767, 'invalid source');
  const start = entries.findLastIndex(entry => entry.name === 'S_START');
  const end = entries.findLastIndex(entry => entry.name === 'S_END');
  if (start < 0 && end < 0) return null;
  demand(start >= 0 && end > start, 'missing or reversed sprite markers');
  for (let index = start + 1; index < end; ++index) {
    const entry = entries[index];
    demand(entry.index === index && Number.isSafeInteger(entry.offset) && Number.isSafeInteger(entry.length)
      && entry.offset >= 0 && entry.length >= 8 && entry.offset <= source.length
      && entry.length <= source.length - entry.offset && entry.length <= 0x7fffffff,
    'truncated patch header in lump ' + index);
    const width = source.readInt16LE(entry.offset), height = source.readInt16LE(entry.offset + 2);
    demand(width > 0 && height > 0 && 8 + width * 4 <= entry.length,
      'invalid patch header bounds in lump ' + index);
    // Match the target patch header's padded columnofs[8] representation.
    demand(4 + Math.max(width, 8) * 2 + entry.length - (8 + width * 4) <= 0x7fffffff,
      'patch too large in lump ' + index);
  }
  return { count: end - start - 1, firstLump: start + 1, lastLump: end - 1 };
}

export function cookSpriteMetrics(source, entries) {
  const range = sourceRange(source, entries);
  if (!range) return null;
  const { count, firstLump, lastLump } = range, arrayWords = count * 2;
  const data = Buffer.alloc((SPRITE_METRICS_HEADER_WORDS + arrayWords * 3) * 2);
  [0x534d, 1, count, firstLump, lastLump, 2, arrayWords & 65535, arrayWords >>> 16]
    .forEach((value, word) => data.writeUInt16LE(value, word * 2));
  for (let i = 0; i < count; ++i) {
    const at = entries[firstLump + i].offset;
    for (const [array, field] of [[0, 0], [1, 4], [2, 6]])
      data.writeInt32LE(source.readInt16LE(at + field) * 65536,
        SPRITE_METRICS_HEADER_WORDS * 2 + (array * count + i) * 4);
  }
  return { data, ...range };
}

// Independent readback compares the two native cells directly against each
// signed source word. It does not call the packer or reproduce its multiply.
export function validateSpriteMetrics(data, source, entries) {
  const range = sourceRange(source, entries);
  if (data == null || Buffer.isBuffer(data) && data.length === 0) {
    demand(range === null, 'missing section for sprite markers');
    return { count: 0 };
  }
  demand(range !== null, 'section without sprite markers');
  const { count, firstLump, lastLump } = range, arrayWords = count * 2;
  demand(Buffer.isBuffer(data) && data.length === (SPRITE_METRICS_HEADER_WORDS + arrayWords * 3) * 2,
    'section length');
  [0x534d, 1, count, firstLump, lastLump, 2, arrayWords & 65535, arrayWords >>> 16, 0, 0, 0, 0, 0, 0, 0, 0]
    .forEach((value, word) => demand(data.readUInt16LE(word * 2) === value, 'header word ' + word));
  for (let array = 0; array < 3; ++array) for (let i = 0; i < count; ++i) {
    const at = SPRITE_METRICS_HEADER_WORDS * 2 + (array * count + i) * 4;
    const field = array === 0 ? 0 : array === 1 ? 4 : 6;
    demand(data.readUInt16LE(at) === 0
      && data.readInt16LE(at + 2) === source.readInt16LE(entries[firstLump + i].offset + field),
    'array ' + array + ', sprite ' + i);
  }
  return range;
}
