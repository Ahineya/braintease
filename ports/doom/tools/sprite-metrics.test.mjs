import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { cookSpriteMetrics, validateSpriteMetrics } from './sprite-metrics.mjs';

function fixture(values) {
  const source = Buffer.alloc(values.length * 32);
  const entries = [{ index: 0, name: 'S_START', offset: 0, length: 0 }];
  values.forEach(([width, left, top], i) => {
    const offset = i * 32;
    source.writeInt16LE(width, offset); source.writeInt16LE(1, offset + 2);
    source.writeInt16LE(left, offset + 4); source.writeInt16LE(top, offset + 6);
    entries.push({ index: i + 1, name: 'JUNK' + i, offset, length: 32 });
  });
  entries.push({ index: entries.length, name: 'S_END', offset: 0, length: 0 });
  return { source, entries };
}
function checked({ source, entries }) {
  const packed = cookSpriteMetrics(source, entries), { data, ...summary } = packed;
  assert.deepEqual(validateSpriteMetrics(data, source, entries), summary);
  return packed;
}
test('all namespace lumps are included, preserving signed 16-bit origin extremes', () => {
  const packed = checked(fixture([[1, -32768, 32767], [2, -1, 0], [3, 32767, -32768]]));
  assert.equal(packed.count, 3);
  assert.equal(packed.data.readInt32LE(32), 65536);
  assert.equal(packed.data.readInt32LE(32 + 3 * 4), -2147483648);
  assert.equal(packed.data.readInt32LE(32 + 8 * 4), -2147483648);
});
test('largest positive width remains a signed 16.16 value', () => {
  const input = fixture([[1, 0, 0]]);
  input.source = Buffer.alloc(8 + 32767 * 4);
  input.source.writeInt16LE(32767, 0); input.source.writeInt16LE(32767, 2);
  input.entries[1].length = input.source.length;
  assert.equal(checked(input).data.readInt32LE(32), 2147418112);
});
test('empty namespaces and absent sections remain distinct', () => {
  const empty = fixture([]), packed = checked(empty);
  assert.equal(packed.count, 0); assert.equal(packed.data.length, 32);
  assert.equal(cookSpriteMetrics(Buffer.alloc(0), []), null);
  assert.deepEqual(validateSpriteMetrics(null, Buffer.alloc(0), []), { count: 0 });
  assert.throws(() => validateSpriteMetrics(null, empty.source, empty.entries), /missing section/);
  assert.throws(() => validateSpriteMetrics(packed.data, Buffer.alloc(0), []), /without sprite markers/);
  for (const entries of [[empty.entries[0]], [empty.entries[1]], [...empty.entries].reverse()])
    assert.throws(() => cookSpriteMetrics(empty.source, entries), /markers/);
});
test('every serialized header and metric cell is checked against original headers', () => {
  const input = fixture([[1, -32768, 32767], [2, -1, 0]]), packed = checked(input);
  for (let word = 0; word < packed.data.length / 2; ++word) {
    const mutant = Buffer.from(packed.data);
    mutant.writeUInt16LE((mutant.readUInt16LE(word * 2) + 1) & 65535, word * 2);
    assert.throws(() => validateSpriteMetrics(mutant, input.source, input.entries), /Invalid sprite metrics:/, 'word ' + word);
  }
  for (const length of [0, 1, 31, packed.data.length - 1, packed.data.length - 2])
    assert.throws(() => validateSpriteMetrics(packed.data.subarray(0, length), input.source, input.entries), /Invalid sprite metrics:/);
});
test('last marker pair wins and patch headers outside the selected range are ignored', () => {
  const input = fixture([[1, -1, 2]]);
  input.entries.unshift({ name: 'S_START' }, { name: 'IGNORED' }, { name: 'S_END' });
  input.entries.forEach((entry, index) => entry.index = index);
  const result = checked(input);
  assert.equal(result.firstLump, 4); assert.equal(result.lastLump, 4);
});
test('source bounds and dimensions are rejected even for unrecognized sprite names', () => {
  for (const mutate of [
    ({ entries }) => entries[1].length = 7,
    ({ entries }) => entries[1].offset = -1,
    ({ entries }) => entries[1].offset = 31,
    ({ entries }) => entries[1].length = 33,
    ({ entries }) => entries[1].index = 2,
    ({ source }) => source.writeInt16LE(0, 0),
    ({ source }) => source.writeInt16LE(-1, 0),
    ({ source }) => source.writeInt16LE(7, 0),
    ({ source }) => source.writeInt16LE(0, 2),
    ({ source }) => source.writeInt16LE(-1, 2),
  ]) {
    const input = fixture([[1, 0, 0]]); mutate(input);
    assert.throws(() => cookSpriteMetrics(input.source, input.entries), /Invalid sprite metrics:/);
  }
});
test('contiguous metrics span 64k-cell boundaries without truncating counts', () => {
  const input = fixture(Array.from({ length: 11001 }, (_, i) => [1, (i % 65536) - 32768, 32767]));
  const result = checked(input);
  assert.equal(result.count, 11001); assert.equal(result.data.length / 2, 66022);
});
test('shareware metrics exactly match all 483 original sprite patch headers', () => {
  const source = fs.readFileSync(new URL('../assets/doom1.wad', import.meta.url));
  const count = source.readUInt32LE(4), directory = source.readUInt32LE(8);
  const entries = Array.from({ length: count }, (_, index) => {
    const at = directory + index * 16;
    return { index, offset: source.readUInt32LE(at), length: source.readUInt32LE(at + 4),
      name: source.subarray(at + 8, at + 16).toString('latin1').replace(/\0.*/, '').toUpperCase() };
  });
  const result = checked({ source, entries });
  assert.equal(result.count, 483); assert.equal(result.data.length / 2, 2914);
});
