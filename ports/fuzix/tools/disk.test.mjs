import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import {buildDisk, rootImage} from './disk.mjs';
import {fatFile} from './fat.mjs';
import {checkRoot} from './fsck.mjs';

const seed = [{name: '/init', data: Buffer.alloc(75000, 0x61)}];
test('C16 root has valid double-indirect files, directory links and free lists', () => {
  const image = rootImage(seed);
  assert.equal(checkRoot(image).issues.length, 0);
  assert.deepEqual(image, rootImage(seed));
});
test('checker repairs dirty/free-list metadata but rejects ambiguous structural corruption', () => {
  const image = rootImage(seed);
  image.writeUInt16LE(1, (256 + 105) * 2);
  image.writeUInt16LE(999, (256 + 109) * 2);
  assert.equal(checkRoot(image).issues.length, 2);
  const fixed = checkRoot(image, true).image;
  assert.deepEqual(checkRoot(fixed).issues, []);
  const broken = Buffer.from(image);
  broken.writeUInt16LE(2, (2 * 256 + 32 + 12) * 2);
  assert.throws(() => checkRoot(broken, true), /outside data area/);
});
test('kernel updates preserve every root byte and no-op updates preserve disk timestamps', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-disk-'));
  try {
    for (const name of ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'])
      fs.writeFileSync(path.join(dir, `${name}.rxe`), Buffer.from(name));
    fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'first kernel');
    const disk = path.join(dir, 'disk.img');
    buildDisk(dir, disk);
    let image = fs.readFileSync(disk), root = fatFile(image, 'ROOTFS.IMG');
    root.data[450] = 0x5a;
    image = root.replace(root.data);
    fs.writeFileSync(disk, image);
    const expected = fatFile(image, 'ROOTFS.IMG').data;
    fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), Buffer.alloc(24000, 0x5e));
    assert.equal(buildDisk(dir, disk), true);
    assert.deepEqual(fatFile(fs.readFileSync(disk), 'ROOTFS.IMG').data, expected);
    assert.deepEqual(fs.readFileSync(`${disk}.bak`), image);
    const before = fs.statSync(disk).mtimeMs;
    assert.equal(buildDisk(dir, disk), false);
    assert.equal(fs.statSync(disk).mtimeMs, before);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});
test('FAT mapping refuses truncated or circular chains and replacement resizing', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-fat-'));
  try {
    for (const name of ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'])
      fs.writeFileSync(path.join(dir, `${name}.rxe`), 'seed');
    fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'kernel');
    const disk = path.join(dir, 'disk.img');
    buildDisk(dir, disk);
    const image = fs.readFileSync(disk);
    assert.throws(() => fatFile(image, 'ROOTFS.IMG').replace(Buffer.alloc(1)), /resize/);
    assert.throws(() => fatFile(image.subarray(0, 1000), 'ROOTFS.IMG'));
    const cyclic = Buffer.from(image), root = 512 + 2 * 32 * 512,
          first = cyclic.readUInt16LE(root + 32 + 26);
    for (let n = 0; n < 2; n++) cyclic.writeUInt16LE(first, 512 + n * 32 * 512 + first * 2);
    assert.throws(() => fatFile(cyclic, 'ROOTFS.IMG'), /chain/);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});
