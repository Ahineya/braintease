import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { fat16Image, updateFat16Image } from './mkfat16.mjs';

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
  assert.equal(image.subarray(43, 54).toString(), 'RIPPLEOS  \0');
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

test('explicit larger clusters retain file data and reject invalid sizes', () => {
  const data = Buffer.alloc(20001, 0xa7);
  const image = fat16Image([{name: 'DATA.BIN', data}],
      {totalSectors: 65536, sectorsPerFat: 32, sectorsPerCluster: 16});
  assert.equal(image[13], 16);
  assert.deepEqual(rootFiles(image).get('DATA    BIN').data, data);
  assert.deepEqual(rootFiles(updateFat16Image(image, [{name: 'DATA.BIN', data: Buffer.from('changed')}]))
      .get('DATA    BIN').data, Buffer.from('changed'));
  for (const size of [0, 3, 256, 1.5])
    assert.throws(() => fat16Image([], {sectorsPerCluster: size}), /geometry/);
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

function rootFiles(image) {
  const bps = image.readUInt16LE(11);
  const bytes = bps * image[13];
  const fatStart = image.readUInt16LE(14) * bps;
  const rootStart = fatStart + image[16] * image.readUInt16LE(22) * bps;
  const count = image.readUInt16LE(17);
  const dataStart = rootStart + Math.ceil(count * 32 / bps) * bps;
  const result = new Map();
  for (let entry = rootStart; entry < rootStart + count * 32 && image[entry]; entry += 32) {
    if (image[entry] === 0xe5) continue;
    const name = image.subarray(entry, entry + 11).toString('ascii');
    const chain = [];
    const chunks = [];
    for (let c = image.readUInt16LE(entry + 26); c && c < 0xfff8; c = image.readUInt16LE(fatStart + c * 2)) {
      assert.ok(!chain.includes(c));
      chain.push(c);
      chunks.push(Buffer.from(image.subarray(dataStart + (c - 2) * bytes, dataStart + (c - 1) * bytes)));
    }
    result.set(name, {
      entry, chain, chunks,
      metadata: Buffer.from(image.subarray(entry, entry + 32)),
      data: Buffer.concat(chunks).subarray(0, image.readUInt32LE(entry + 28)),
    });
  }
  return result;
}

test('OS update preserves unrelated files, directories, metadata and allocated clusters', () => {
  const before = fat16Image([
    { name: 'IO.SYS', data: Buffer.alloc(5000, 1) },
    { name: 'NOTES.TXT', data: Buffer.alloc(7000, 2) },
    { name: 'KERNEL.SYS', data: Buffer.alloc(3000, 3) },
    { name: 'MYDIR', data: Buffer.alloc(2048, 0) },
  ]);
  const entries = rootFiles(before);
  before[entries.get('MYDIR      ').entry + 11] = 0x10;
  before.writeUInt32LE(0x12345678, entries.get('NOTES   TXT').entry + 14);
  const original = Buffer.from(before);
  const io = Buffer.alloc(15000, 42);
  const hello = Buffer.from('new app');
  const after = updateFat16Image(before, [
    { name: 'IO.SYS', data: io },
    { name: 'KERNEL.SYS', data: Buffer.alloc(0) },
    { name: 'HELLO.RXE', data: hello },
  ]);
  assert.deepEqual(before, original, 'the input must not be mutated');
  const oldFiles = rootFiles(before);
  const newFiles = rootFiles(after);
  for (const name of ['NOTES   TXT', 'MYDIR      ']) {
    assert.deepEqual(newFiles.get(name), oldFiles.get(name));
  }
  assert.deepEqual(newFiles.get('IO      SYS').data, io);
  assert.equal(newFiles.get('KERNEL  SYS').chain.length, 0);
  assert.deepEqual(newFiles.get('HELLO   RXE').data, hello);
  const fatStart = before.readUInt16LE(14) * 512;
  const fatBytes = before.readUInt16LE(22) * 512;
  assert.deepEqual(after.subarray(fatStart, fatStart + fatBytes), after.subarray(fatStart + fatBytes, fatStart + 2 * fatBytes));
  assert.deepEqual(updateFat16Image(after, [
    { name: 'IO.SYS', data: io },
    { name: 'KERNEL.SYS', data: Buffer.alloc(0) },
    { name: 'HELLO.RXE', data: hello },
  ]), after, 'repeating the same update must be stable');
});

test('invalid chains, inconsistent FATs, directories and full disks fail without mutation', () => {
  const source = fat16Image([{ name: 'IO.SYS', data: Buffer.alloc(2048) }]);
  const original = Buffer.from(source);
  assert.throws(() => updateFat16Image(source, [{ name: 'IO.SYS', data: Buffer.alloc(10 * 1024 * 1024) }]), /space/);
  assert.deepEqual(source, original);
  const corrupt = Buffer.from(source);
  for (const start of [512, 512 + 32 * 512]) corrupt.writeUInt16LE(2, start + 4);
  assert.throws(() => updateFat16Image(corrupt, [{ name: 'IO.SYS', data: Buffer.alloc(1) }]), /chain/);
  const disagree = Buffer.from(source);
  disagree[512 + 4] = 0;
  assert.throws(() => updateFat16Image(disagree, []), /copies disagree/);
  const directory = Buffer.from(source);
  directory[rootFiles(source).get('IO      SYS').entry + 11] = 0x10;
  assert.throws(() => updateFat16Image(directory, [{ name: 'IO.SYS', data: Buffer.alloc(1) }]), /regular root file/);
});

test('CLI updates atomically with backups and skips unchanged disks', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ripple-fat16-update-'));
  try {
    const disk = path.join(directory, 'ros.img');
    const code = path.join(directory, 'IO.SYS');
    const initial = fat16Image([
      { name: 'IO.SYS', data: Buffer.from('old') },
      { name: 'USER.TXT', data: Buffer.from('keep me') },
    ]);
    fs.writeFileSync(disk, initial);
    fs.writeFileSync(code, Buffer.from('new kernel code'));
    const command = fileURLToPath(new URL('./mkfat16.mjs', import.meta.url));
    const update = () => spawnSync(process.execPath, [command, '--update', '--backup', '-o', disk, code], { encoding: 'utf8' });
    const first = update();
    assert.equal(first.status, 0, first.stderr);
    assert.deepEqual(fs.readFileSync(`${disk}.bak`), initial);
    assert.equal(rootFiles(fs.readFileSync(disk)).get('USER    TXT').data.toString(), 'keep me');
    const second = update();
    assert.equal(second.status, 0, second.stderr);
    assert.match(second.stdout, /unchanged/);
    assert.ok(!fs.existsSync(`${disk}.bak.1`));
    fs.writeFileSync(code, Buffer.alloc(10 * 1024 * 1024));
    const beforeFailure = fs.readFileSync(disk);
    assert.notEqual(update().status, 0);
    assert.deepEqual(fs.readFileSync(disk), beforeFailure);
    fs.writeFileSync(code, Buffer.from('another revision'));
    assert.equal(update().status, 0);
    assert.deepEqual(fs.readFileSync(`${disk}.bak.1`), beforeFailure);
    assert.ok(!fs.readdirSync(directory).some(name => name.includes('.tmp-')));
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
