import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { fat16Image } from './mkfat16.mjs';

test('BPB, both FAT copies, root entries and cluster chains preserve file data', () => {
  const files = [0, 1, 2048, 2049, 7777].map((length, index) => ({
    name: index === 4 ? 'longfilename.longextension' : `file${index}.bin`,
    data: Buffer.from(Array.from({ length }, (_, offset) => (offset * 37 + index) & 255)),
  }));
  const image = fat16Image(files);
  assert.equal(image.length, 10 * 1024 * 1024);
  assert.equal(image.readUInt16LE(11), 512);
  assert.equal(image[13], 4);
  assert.equal(image.readUInt16LE(14), 1);
  assert.equal(image[16], 2);
  assert.equal(image.readUInt16LE(17), 512);
  assert.equal(image.readUInt16LE(19), 20480);
  assert.equal(image.readUInt16LE(22), 32);
  assert.equal(image.readUInt32LE(32), 20480);
  assert.equal(image.readUInt32LE(39), 0x12345678);
  assert.equal(image.subarray(43, 54).toString(), 'RIPPLEROS  ');
  assert.equal(image.subarray(54, 62).toString(), 'FAT16   ');
  assert.equal(image.readUInt16LE(510), 0xaa55);

  const sectorBytes = image.readUInt16LE(11);
  const clusterBytes = image[13] * sectorBytes;
  const fatStart = image.readUInt16LE(14) * sectorBytes;
  const fatBytes = image.readUInt16LE(22) * sectorBytes;
  const rootStart = fatStart + image[16] * fatBytes;
  const dataStart = rootStart + image.readUInt16LE(17) * 32;
  const fat = image.subarray(fatStart, fatStart + fatBytes);
  assert.deepEqual(fat, image.subarray(fatStart + fatBytes, fatStart + 2 * fatBytes));
  assert.equal(fat.readUInt16LE(0), 0xfff8);
  assert.equal(fat.readUInt16LE(2), 0xffff);
  const seen = new Set();
  for (const [index, file] of files.entries()) {
    const entry = rootStart + index * 32;
    const expectedName = index === 4 ? 'LONGFILELON' : `FILE${index}   BIN`;
    assert.equal(image.subarray(entry, entry + 11).toString(), expectedName);
    assert.equal(image[entry + 11], 0x20);
    assert.equal(image.readUInt32LE(entry + 28), file.data.length);
    let cluster = image.readUInt16LE(entry + 26);
    if (!file.data.length) {
      assert.equal(cluster, 0);
      continue;
    }
    const chunks = [];
    while (cluster < 0xfff8) {
      assert.ok(cluster >= 2 && cluster < fatBytes / 2);
      assert.ok(!seen.has(cluster), 'chains must be acyclic and must not share clusters');
      seen.add(cluster);
      const start = dataStart + (cluster - 2) * clusterBytes;
      assert.ok(start + clusterBytes <= image.length);
      chunks.push(image.subarray(start, start + clusterBytes));
      cluster = fat.readUInt16LE(cluster * 2);
    }
    const storage = Buffer.concat(chunks);
    assert.equal(storage.length, Math.ceil(file.data.length / clusterBytes) * clusterBytes);
    assert.deepEqual(storage.subarray(0, file.data.length), file.data);
    assert.ok(storage.subarray(file.data.length).every(byte => byte === 0));
  }
  for (let cluster = 2; cluster < fatBytes / 2; ++cluster) {
    if (!seen.has(cluster)) assert.equal(fat.readUInt16LE(cluster * 2), 0);
  }
  assert.ok(image.subarray(rootStart + files.length * 32, dataStart).every(byte => byte === 0));
});

test('empty images and capacity boundaries remain valid', () => {
  const empty = fat16Image([]);
  assert.ok(empty.subarray(65 * 512).every(byte => byte === 0));
  assert.throws(() => fat16Image(Array.from({ length: 513 }, () => ({ name: 'X', data: Buffer.alloc(0) }))), /root directory/);
  assert.throws(() => fat16Image([{ name: 'BIG', data: { length: 10 * 1024 * 1024 } }]), /capacity/);
  assert.throws(() => fat16Image([{ name: 'caf\u00e9.bin', data: Buffer.alloc(0) }]), /ASCII/);
});

test('CLI creates parent directories and accepts basename and explicit names', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ripple-fat16-'));
  try {
    const source = path.join(directory, 'sample.bin');
    const explicit = path.join(directory, 'other file.dat');
    const output = path.join(directory, 'nested', 'disk.img');
    fs.writeFileSync(source, Buffer.from([1, 2, 3]));
    fs.writeFileSync(explicit, Buffer.from([4, 5]));
    const command = fileURLToPath(new URL('./mkfat16.mjs', import.meta.url));
    const result = spawnSync(process.execPath, [command, '-o', output, source, `${explicit}:IO.SYS`], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.deepEqual(fs.readFileSync(output), fat16Image([
      { name: 'sample.bin', data: Buffer.from([1, 2, 3]) },
      { name: 'IO.SYS', data: Buffer.from([4, 5]) },
    ]));
    const invalid = spawnSync(process.execPath, [command, '--output'], { encoding: 'utf8' });
    assert.notEqual(invalid.status, 0);
    assert.match(invalid.stderr, /requires a filename/);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
