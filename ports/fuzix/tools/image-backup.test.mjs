import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {backupImage, pruneImageBackups, imageBackups, backupLimit} from './image-backup.mjs';
import {Image, openImage, writeImage} from './image.mjs';

function temporary(fn) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'image-retention-'));
  try { fn(directory); } finally { fs.rmSync(directory, {recursive: true, force: true}); }
}
test('repeated upgrades retain exactly the previous sparse image and prune legacy snapshots', () => temporary(dir => {
  const filename = path.join(dir, 'disk.img');
  const image = new Image(1024 ** 3);
  image.writeUInt16LE(10, image.length - 2);
  writeImage(filename, image);
  fs.writeFileSync(filename + '.bak.39', 'obsolete');
  fs.writeFileSync(filename + '.bak.images-old', 'obsolete');
  fs.writeFileSync(path.join(dir, 'notes.bak'), 'keep');
  for (let value = 11; value < 18; value++) {
    const original = openImage(filename);
    assert.equal(backupImage(filename, original, {keep: 1}), filename + '.bak');
    const updated = original.fork();updated.writeUInt16LE(value, image.length - 2);
    const next = path.join(dir, 'next');writeImage(next, updated);fs.renameSync(next, filename);
    pruneImageBackups(filename, {keep: 1});
    assert.deepEqual(imageBackups(filename), [filename + '.bak']);
    assert.equal(openImage(filename + '.bak').readUInt16LE(image.length - 2), value - 1);
    assert.equal(openImage(filename).readUInt16LE(image.length - 2), value);
    assert.ok(fs.statSync(filename + '.bak').blocks * 512 < 1024 ** 2);
  }
  assert.equal(fs.readFileSync(path.join(dir, 'notes.bak'), 'utf8'), 'keep');
  assert.ok(!fs.readdirSync(dir).some(n => n.startsWith('.image-backup-')));
}));
test('configurable retention and disabled backups remain bounded', () => temporary(dir => {
  const filename = path.join(dir, 'root.img');
  for (let value = 0; value < 6; value++) {
    backupImage(filename, Buffer.from(String(value)), {keep: 2});
    pruneImageBackups(filename, {keep: 2});
    assert.ok(imageBackups(filename).length <= 2);
  }
  assert.equal(fs.readFileSync(filename + '.bak', 'utf8'), '5');
  assert.equal(backupImage(filename, Buffer.from('unused'), {keep: 0}), null);
  pruneImageBackups(filename, {keep: 0});
  assert.deepEqual(imageBackups(filename), []);
}));
test('failed staging preserves the disk and last recovery copy', () => temporary(dir => {
  const filename = path.join(dir, 'root.img');
  fs.writeFileSync(filename, 'current');fs.writeFileSync(filename + '.bak', 'previous');
  assert.throws(() => backupImage(filename, {}, {keep: 1}));
  assert.equal(fs.readFileSync(filename, 'utf8'), 'current');
  assert.equal(fs.readFileSync(filename + '.bak', 'utf8'), 'previous');
  assert.deepEqual(fs.readdirSync(dir).sort(), ['root.img', 'root.img.bak']);
  for (const invalid of ['-1', '100', 'abc', '1.5']) assert.throws(() => backupLimit(invalid));
}));
