// SPDX-License-Identifier: GPL-2.0-or-later
// Read back the serialized section independently of the packing routine.
// Source constants are shared; reference arithmetic and traversal are separate.
import { rendererSourceTables } from './renderer-tables.mjs';

export function validateRendererTables(buffer) {
  const fail = message => { throw new Error('Invalid renderer tables: ' + message); };
  if (!Buffer.isBuffer(buffer) || buffer.length % 2 || buffer.length < 608) fail('section length');
  const cells = buffer.length / 2;
  const u16 = at => {
    if (!Number.isSafeInteger(at) || at < 0 || at >= cells) fail('cell outside section');
    return buffer.readUInt16LE(at * 2);
  };
  const u32 = at => u16(at) + u16(at + 1) * 65536;
  const i32 = at => u32(at) | 0;
  const equal = (a, b, name) => { if (a !== b) fail(name); };
  const div = (a, b) => {
    const aa = BigInt(a), bb = BigInt(b), abs = x => x < 0n ? -x : x;
    if (abs(aa) / 16384n >= abs(bb)) return (aa < 0n) !== (bb < 0n) ? -2147483648 : 2147483647;
    return Number(aa * 65536n / bb);
  };
  const mul = (a, b) => Number(BigInt.asIntN(32, BigInt(a) * BigInt(b) >> 16n));
  const light = n => n < 0 ? 0 : n > 31 ? 31 : n;
  equal(u16(0), 0x5256, 'magic'); equal(u16(1), 0x5442, 'magic');
  equal(u16(2), 1, 'version'); equal(u16(3), 18, 'view count');
  equal(u32(4), 304, 'depth table offset'); equal(u16(6), 2048, 'depth table count');
  for (let n = 7; n < 16; n++) equal(u16(n), 0, 'reserved header');
  let cursor = 304;
  for (let row = 0; row < 16; row++) for (let col = 0; col < 128; col++) {
    const scale = Math.floor(div(10485760, (col + 1) * 1048576) / 4096);
    equal(u16(cursor++), light((15 - row) * 4 - Math.trunc(scale / 2)), 'depth light value');
  }
  const { tangent, sine } = rendererSourceTables();
  for (let view = 0; view < 18; view++) {
    const record = 16 + view * 16, blocks = 3 + Math.floor(view / 2), detail = view % 2;
    const scaled = blocks === 11 ? 320 : blocks * 32;
    const height = blocks === 11 ? 200 : Math.floor(blocks * 168 / 10 / 8) * 8;
    const width = scaled / (detail + 1);
    [blocks, detail, scaled, height, width, 0].forEach((value, n) => equal(u16(record + n), value, 'view dimensions'));
    const offsets = Array.from({ length: 5 }, (_, n) => u32(record + 6 + n * 2));
    const lengths = [4096, (width + 1) * 2, height * 2, width * 2, 768];
    offsets.forEach((offset, n) => { equal(offset, cursor, 'table extent'); cursor += lengths[n]; });
    const center = width / 2 * 65536, focal = div(center, tangent[3072]);
    const projected = tangent.map(t => t > 131072 ? -1 : t < -131072 ? width + 1
      : Math.max(-1, Math.min(width + 1, Math.floor((center - mul(t, focal) + 65535) / 65536))));
    for (let angle = 0; angle < 4096; angle++)
      equal(u16(offsets[0] + angle), Math.max(0, Math.min(width, projected[angle])), 'angle projection');
    // Preserve the original independent search for each x in this oracle.
    const angles = [];
    for (let x = 0; x <= width; x++) {
      let angle = 0;
      while (projected[angle] > x) angle++;
      const result = (angle * 524288 - 1073741824) >>> 0;
      equal(u32(offsets[1] + x * 2), result, 'inverse projection'); angles.push(result);
    }
    for (let y = 0; y < height; y++) equal(i32(offsets[2] + y * 2),
      div(scaled / 2 * 65536, Math.abs((y - height / 2) * 65536 + 32768)), 'plane slope');
    for (let x = 0; x < width; x++) equal(i32(offsets[3] + x * 2),
      div(65536, Math.abs(sine[2048 + Math.floor(angles[x] / 524288)])), 'distance scale');
    for (let row = 0; row < 16; row++) for (let col = 0; col < 48; col++)
      equal(u16(offsets[4] + row * 48 + col),
        light((15 - row) * 4 - Math.trunc(Math.trunc(col * 320 / scaled) / 2)), 'scale light value');
  }
  equal(cursor, cells, 'trailing cells');
  return { views: 18, words: cells };
}
