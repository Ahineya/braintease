import test from 'node:test';
import assert from 'node:assert/strict';
import { packRendererTables, rendererFixedDiv, rendererFixedMul } from './renderer-tables.mjs';
import { validateRendererTables } from './renderer-tables-validate.mjs';

const packed = packRendererTables();
test('renderer section contains every supported view and spans a memory bank', () => {
  assert.deepEqual(validateRendererTables(packed.buffer), { views: 18, words: packed.words });
  assert.equal(packed.words, 106004);
  assert.ok(packed.buffer.readUInt32LE((16 + 17 * 16 + 6) * 2) > 64000);
});

const mutations = [
  ['magic', 0], ['version', 2], ['view count', 3], ['depth offset', 4],
  ['depth count', 6], ['reserved header', 15], ['blocks', 16],
  ['detail', 17], ['scaled width', 18], ['height', 19], ['width', 20],
  ['reserved view', 21], ['projection offset', 22], ['offset high word', 23],
  ['depth light', 304],
];
for (let field = 0; field < 5; field++) for (const view of [0, 17]) {
  const offset = packed.buffer.readUInt32LE((16 + view * 16 + 6 + field * 2) * 2);
  mutations.push(['view ' + view + ' table ' + field, offset]);
}
for (const [name, cell] of mutations) test('rejects corrupted ' + name, () => {
  const corrupt = Buffer.from(packed.buffer);
  corrupt.writeUInt16LE(corrupt.readUInt16LE(cell * 2) ^ 1, cell * 2);
  assert.throws(() => validateRendererTables(corrupt), /Invalid renderer tables:/);
});
test('rejects missing, truncated and trailing cells', () => {
  for (const invalid of [Buffer.alloc(0), packed.buffer.subarray(0, -1),
    packed.buffer.subarray(0, -2), Buffer.concat([packed.buffer, Buffer.alloc(2)])])
    assert.throws(() => validateRendererTables(invalid), /Invalid renderer tables:/);
});
test('fixed division preserves saturation and truncation', () => {
  for (const [a, b, expected] of [[0, 0, 2147483647], [-1, 0, -2147483648],
    [-2147483648, 1, -2147483648], [-2147483648, -1, 2147483647],
    [1, 3, 21845], [-1, 3, -21845], [1, -3, -21845], [-1, -3, 21845],
    [65536, 65536, 65536], [65536, 4, 2147483647]])
    assert.equal(rendererFixedDiv(a, b), expected);
});
test('fixed multiplication preserves negative rounding and wrapping', () => {
  assert.equal(rendererFixedMul(-1, 1), -1);
  assert.equal(rendererFixedMul(-65536, 65536), -65536);
  assert.equal(rendererFixedMul(-2147483648, 131072), 0);
  assert.equal(rendererFixedMul(2147483647, 65536), 2147483647);
});
