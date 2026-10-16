import test from 'node:test';
import assert from 'node:assert/strict';
import {sokobanLevels, sokobanFiles} from './sokoban.mjs';
const title = '#####\n#@Hi#\n#####\n\n';
const puzzle = '######\n#@ $.#\n######\n';
test('C16 levels have checked records and preserve box/goal/player state', () => {
  const data = sokobanLevels(title + puzzle);
  assert.equal(data.length, 1024);
  const p = data.subarray(512);
  assert.equal(p[450], 0); assert.equal(p[451], 1);
  assert.equal(p[p[448]*32+p[449]], 32);
  assert.equal(p.filter(c => c === 36).length, 1);
  assert.equal(p.filter(c => c === 46).length, 1);
});
test('malformed collections are rejected', () => {
  for (const text of [title, title+'#@ $ #\n', title+'#@@$.#\n',
                       title+'#@'+ ' '.repeat(40)+'$.#\n',
                       title+'#@X$.#\n', title+('#@$.#\n'.repeat(15))])
    assert.throws(() => sokobanLevels(text));
});
test('supplied collection retains all 107 puzzles and attribution', () => {
  const files = sokobanFiles();
  assert.equal(files[0].data.length, 108*512);
  assert.match(files[1].data.toString(), /Lee J Haywood/);
  assert.match(files[1].data.toString(), /remain unchanged/);
});
