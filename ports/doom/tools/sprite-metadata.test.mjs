import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { readSpriteNames, spriteNameIdentity, cookSpriteMetadata, validateSpriteMetadata } from './sprite-metadata.mjs';

function entries(names) {
  return names.map((text, index) => {
    const nameBytes = Buffer.alloc(8); nameBytes.write(text, 'latin1');
    return { index, name: text.toUpperCase(), nameBytes };
  });
}
const wrap = names => entries(['S_START', ...names, 'S_END']);
const names = ['TROO', 'POSS', 'SHTG'];
function checked(rows, selected = names) {
  const packed = cookSpriteMetadata(rows, selected);
  const { data, ...summary } = packed;
  assert.deepEqual(validateSpriteMetadata(data, rows, selected), summary);
  return packed;
}

test('maintained sprite names and order are bound to the artifact', () => {
  const actual = readSpriteNames();
  assert.equal(actual.length, 138); assert.equal(actual[0], 'TROO'); assert.equal(actual.at(-1), 'TLP2');
  assert.notDeepEqual(spriteNameIdentity(actual), spriteNameIdentity([...actual].reverse()));
  for (const invalid of [[], ['TROO', 'TROO'], ['troo'], ['ABCDE'], ['ÅBCD']])
    assert.throws(() => spriteNameIdentity(invalid), /invalid sprite name list/);
});

test('rotation-zero, mirrored pairs, absent sprites and ASCII-insensitive prefixes', () => {
  const rows = wrap(['trooA0', 'TROOB1B5', 'TROOB2B6', 'TROOB3B7', 'TROOB4B8', 'POSSA0']);
  const result = checked(rows);
  assert.equal(result.frames, 3);
  const frame = 16 + 3 * names.length;
  assert.equal(result.data.readUInt16LE(frame * 2), 0);
  assert.equal(result.data.readUInt16LE((frame + 17) * 2), 1);
  assert.equal(result.data.readUInt16LE((frame + 17 + 9 + 4) * 2), 1);
  assert.equal(result.data.readUInt16LE((16 + 2 * 3) * 2), 0);
});

test('absent sections and empty sprite ranges remain distinct', () => {
  const absent = entries(['RAW']);
  assert.equal(cookSpriteMetadata(absent), null);
  assert.deepEqual(validateSpriteMetadata(null, absent), { count: 0, frames: 0 });
  const empty = checked(wrap([]));
  assert.equal(empty.frames, 0);
  assert.throws(() => validateSpriteMetadata(null, wrap([])), /missing section/);
  assert.throws(() => validateSpriteMetadata(empty.data, absent, names), /without sprite markers/);
  for (const malformed of [entries(['S_START']), entries(['S_END']), entries(['S_END', 'S_START'])])
    assert.throws(() => cookSpriteMetadata(malformed), /markers/);
});

test('last S_START/S_END pair wins and unrelated malformed names are ignored', () => {
  const rows = entries(['S_START', 'TROOA1', 'S_END', 'S_START', 'JUNKa9', 'TROOA0', 'S_END']);
  const result = checked(rows);
  assert.equal(result.firstLump, 4); assert.equal(result.lastLump, 5); assert.equal(result.frames, 1);
});

test('duplicate frames, missing rotations, frame gaps and malformed suffixes fail offline', () => {
  for (const bad of [['TROOA0', 'TROOA0'], ['TROOA1', 'TROOA1'], ['TROOA0', 'TROOA1'],
    ['TROOA1', 'TROOA0'], ['TROOA1'], ['TROOB0'], ['TROOa0'], ['TROOA9'], ['TROO'],
    ['TROOA0A0'], ['TROOA0A1']])
    assert.throws(() => cookSpriteMetadata(wrap(bad), names), /Invalid sprite metadata:/, bad.join(','));
});

test('every header, definition, mapping and flip cell is checked against raw names', () => {
  const rows = wrap(['TROOA0', 'POSSA1A5', 'POSSA2A6', 'POSSA3A7', 'POSSA4A8']);
  const packed = checked(rows);
  for (let word = 0; word < packed.data.length / 2; ++word) {
    const mutant = Buffer.from(packed.data);
    mutant.writeUInt16LE((mutant.readUInt16LE(word * 2) + 1) & 65535, word * 2);
    assert.throws(() => validateSpriteMetadata(mutant, rows, names), /Invalid sprite metadata:/, 'word ' + word);
  }
  for (const length of [0, 1, 31, packed.data.length - 1, packed.data.length - 2])
    assert.throws(() => validateSpriteMetadata(packed.data.subarray(0, length), rows, names), /Invalid sprite metadata:/);
  assert.throws(() => validateSpriteMetadata(packed.data, rows, [...names].reverse()), /header/);
});

test('maximum maintained sprite frames cross a 64k-cell bank', () => {
  const selected = readSpriteNames(), rows = wrap(selected.flatMap(name =>
    Array.from({ length: 29 }, (_, frame) => name + String.fromCharCode(65 + frame) + '0')));
  const packed = checked(rows, selected);
  assert.equal(packed.frames, 4002); assert.ok(packed.data.length / 2 > 64000);
});

test('shareware sprite metadata is exactly 138 definitions and 261 frames', () => {
  const source = fs.readFileSync(new URL('../assets/doom1.wad', import.meta.url));
  const count = source.readUInt32LE(4), directory = source.readUInt32LE(8);
  const rows = Array.from({ length: count }, (_, index) => {
    const nameBytes = source.subarray(directory + index * 16 + 8, directory + index * 16 + 16);
    return { index, nameBytes, name: nameBytes.toString('latin1').replace(/\0.*/, '').toUpperCase() };
  });
  const packed = checked(rows, readSpriteNames());
  assert.equal(packed.count, 138); assert.equal(packed.frames, 261);
  assert.equal(packed.data.length / 2, 4867);
});
