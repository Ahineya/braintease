#!/usr/bin/env node
// Create a deterministic FAT16 disk image with files in its root directory.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const BPS = 512;
const SPC = 4;
const RESERVED = 1;
const NUM_FATS = 2;
const ROOT_ENT = 512;
const MEDIA = 0xf8;
const SECS_PER_FAT = 32;
const TOTAL_SECS = 20480; // 10 MiB

function canon83(name) {
  const upper = name.toUpperCase();
  const extension = path.extname(upper);
  const base = extension ? upper.slice(0, -extension.length) : upper;
  const result = base.slice(0, 8).padEnd(8) + extension.slice(1, 4).padEnd(3);
  if (!/^[\x20-\x7e]{11}$/.test(result)) {
    throw new Error(`FAT filename must contain ASCII characters: ${name}`);
  }
  return result;
}

export function fat16Image(files, {totalSectors = TOTAL_SECS, sectorsPerFat = SECS_PER_FAT,
                                    sectorsPerCluster = SPC, imageFactory = size => Buffer.alloc(size)} = {}) {
  const rootSecs = Math.ceil(ROOT_ENT * 32 / BPS);
  const fatOffset = RESERVED * BPS;
  const rootOffset = (RESERVED + NUM_FATS * sectorsPerFat) * BPS;
  const dataOffset = rootOffset + rootSecs * BPS;
  const clusterBytes = sectorsPerCluster * BPS;
  const clusterCount = Math.floor((totalSectors * BPS - dataOffset) / clusterBytes);
  if (!Number.isInteger(totalSectors) || !Number.isInteger(sectorsPerFat)
      || totalSectors < 1 || totalSectors > 0xffffffff || sectorsPerFat < 1 || sectorsPerFat > 65535
      || !Number.isInteger(sectorsPerCluster) || sectorsPerCluster < 1 || sectorsPerCluster > 128
      || (sectorsPerCluster & (sectorsPerCluster - 1))
      || clusterCount < 4085 || clusterCount >= 0xffee
      || clusterCount + 2 > sectorsPerFat * BPS / 2) {
    throw new Error('Invalid FAT16 geometry');
  }
  if (files.length > ROOT_ENT) throw new Error(`FAT root directory holds at most ${ROOT_ENT} files`);
  const requiredClusters = files.reduce((total, file) => total + Math.ceil(file.data.length / clusterBytes), 0);
  if (requiredClusters > clusterCount) throw new Error('Files exceed FAT16 image capacity');

  const image = imageFactory(totalSectors * BPS);
  image.set([0xeb, 0x3c, 0x90], 0);
  image.write('MSDOS5.0', 3, 'ascii');
  image.writeUInt16LE(BPS, 11);
  image.writeUInt8(sectorsPerCluster, 13);
  image.writeUInt16LE(RESERVED, 14);
  image.writeUInt8(NUM_FATS, 16);
  image.writeUInt16LE(ROOT_ENT, 17);
  image.writeUInt16LE(totalSectors <= 65535 ? totalSectors : 0, 19);
  image.writeUInt8(MEDIA, 21);
  image.writeUInt16LE(sectorsPerFat, 22);
  image.writeUInt16LE(32, 24);
  image.writeUInt16LE(2, 26);
  // Preserve the existing image layout, including its total-sector mirror.
  image.writeUInt32LE(totalSectors, 32);
  image.writeUInt8(0x80, 36);
  image.writeUInt8(0x29, 38);
  image.writeUInt32LE(0x12345678, 39);
  image.write('RIPPLEOS  ', 43, 'ascii');
  image.write('FAT16   ', 54, 'ascii');
  image.writeUInt16LE(0xaa55, 510);

  function fatSet(cluster, value) {
    for (let fat = 0; fat < NUM_FATS; ++fat) {
      image.writeUInt16LE(value, fatOffset + fat * sectorsPerFat * BPS + cluster * 2);
    }
  }
  fatSet(0, 0xff00 | MEDIA);
  fatSet(1, 0xffff);

  let nextCluster = 2;
  for (const [index, { name, data }] of files.entries()) {
    const first = data.length ? nextCluster : 0;
    for (let offset = 0; offset < data.length; offset += clusterBytes) {
      const cluster = nextCluster++;
      image.set(data.subarray(offset, offset + clusterBytes), dataOffset + (cluster - 2) * clusterBytes);
      fatSet(cluster, offset + clusterBytes < data.length ? nextCluster : 0xffff);
    }
    const entry = rootOffset + index * 32;
    image.write(canon83(name), entry, 'ascii');
    image.writeUInt8(0x20, entry + 11);
    image.writeUInt16LE(first, entry + 26);
    image.writeUInt32LE(data.length, entry + 28);
  }
  return image;
}

// Replace root files without reformatting: directories, unrelated chains,
// metadata and unused sectors retain their original bytes. Work on a copy so
// validation/allocation failures cannot partially modify the source image.
export function updateFat16Image(source, files) {
  const image = Buffer.isBuffer(source) ? Buffer.from(source) : source.fork();
  if (image.length < 512 || image.readUInt16LE(510) !== 0xaa55) {
    throw new Error('Not a FAT16 disk image');
  }
  const bps = image.readUInt16LE(11);
  const spc = image.readUInt8(13);
  const reserved = image.readUInt16LE(14);
  const fats = image.readUInt8(16);
  const entries = image.readUInt16LE(17);
  const sectors = image.readUInt16LE(19) || image.readUInt32LE(32);
  const fatBytes = image.readUInt16LE(22) * bps;
  const fatStart = reserved * bps;
  const rootStart = fatStart + fats * fatBytes;
  const rootEnd = rootStart + entries * 32;
  const dataStart = rootStart + Math.ceil(entries * 32 / bps) * bps;
  const clusterBytes = bps * spc;
  const clusters = Math.floor((sectors * bps - dataStart) / clusterBytes);
  const clusterLimit = Math.min(clusters + 2, 0xfff0);
  if (![512, 1024, 2048, 4096].includes(bps) || !spc || (spc & (spc - 1))
      || !reserved || !fats || !entries || !fatBytes || sectors * bps > image.length
      || rootEnd > dataStart || dataStart > image.length || clusters < 4085
      || clusters >= 65525 || (clusters + 2) * 2 > fatBytes) {
    throw new Error('Unsupported or corrupt FAT16 layout');
  }
  const fat = image.subarray(fatStart, fatStart + fatBytes);
  for (let index = 1; index < fats; ++index) {
    if (!fat.equals(image.subarray(fatStart + index * fatBytes, fatStart + (index + 1) * fatBytes))) {
      throw new Error('FAT copies disagree; refusing to update');
    }
  }
  function fatSet(cluster, value) {
    fat.writeUInt16LE(value, cluster * 2);
    for (let index = 0; index < fats; ++index) {
      image.writeUInt16LE(value, fatStart + index * fatBytes + cluster * 2);
    }
  }
  const names = new Set();
  const released = new Set();
  const replacements = files.map(file => {
    const name = canon83(file.name);
    if (names.has(name)) throw new Error(`Duplicate replacement: ${file.name}`);
    names.add(name);
    let slot;
    let freeSlot;
    for (let entry = rootStart; entry < rootEnd; entry += 32) {
      const first = image.readUInt8(entry);
      if (first === 0 || first === 0xe5) freeSlot ??= entry;
      if (first === 0) break;
      if (first !== 0xe5 && image.subarray(entry, entry + 11).toString('ascii') === name) {
        if (image.readUInt8(entry + 11) & 0x18) throw new Error(`Not a regular root file: ${file.name}`);
        slot = entry;
        break;
      }
    }
    if (slot === undefined) {
      if (freeSlot === undefined) throw new Error('FAT root directory is full');
      slot = freeSlot;
      const wasEnd = image.readUInt8(slot) === 0;
      image.fill(0, slot, slot + 32);
      if (wasEnd && slot + 32 < rootEnd) image.writeUInt8(0, slot + 32);
      image.write(name, slot, 'ascii');
    } else {
      let cluster = image.readUInt16LE(slot + 26);
      let count = 0;
      while (cluster && cluster < 0xfff8) {
        if (cluster < 2 || cluster >= clusterLimit || released.has(cluster)) {
          throw new Error(`Invalid or shared cluster chain: ${file.name}`);
        }
        released.add(cluster);
        cluster = fat.readUInt16LE(cluster * 2);
        ++count;
        if (!cluster) throw new Error(`Broken cluster chain: ${file.name}`);
      }
      if (count * clusterBytes < image.readUInt32LE(slot + 28)) {
        throw new Error(`Short cluster chain: ${file.name}`);
      }
    }
    return { ...file, slot };
  });
  for (const cluster of released) fatSet(cluster, 0);
  let candidate = 2;
  for (const { data, slot } of replacements) {
    let first = 0;
    let previous = 0;
    for (let offset = 0; offset < data.length; offset += clusterBytes) {
      while (candidate < clusterLimit && fat.readUInt16LE(candidate * 2)) ++candidate;
      if (candidate >= clusterLimit) throw new Error('Files exceed available FAT16 space');
      const cluster = candidate++;
      first ||= cluster;
      if (previous) fatSet(previous, cluster);
      fatSet(cluster, 0xffff);
      const start = dataStart + (cluster - 2) * clusterBytes;
      image.fill(0, start, start + clusterBytes);
      image.set(data.subarray(offset, offset + clusterBytes), start);
      previous = cluster;
    }
    image.writeUInt8(image.readUInt8(slot + 11) | 0x20, slot + 11);
    image.writeUInt16LE(first, slot + 26);
    image.writeUInt32LE(data.length, slot + 28);
  }
  return image;
}

function main(args) {
  let output;
  let update = false;
  let backup = false;
  const inputs = [];
  let positionalOnly = false;
  for (let i = 0; i < args.length; ++i) {
    const arg = args[i];
    if (!positionalOnly && arg === '--') {
      positionalOnly = true;
    } else if (!positionalOnly && (arg === '-h' || arg === '--help')) {
      console.log('Usage: node mkfat16.mjs [--update [--backup]] -o IMAGE [src:DESTNAME | src ...]');
      return;
    } else if (!positionalOnly && arg === '--update') {
      update = true;
    } else if (!positionalOnly && arg === '--backup') {
      backup = true;
    } else if (!positionalOnly && (arg === '-o' || arg === '--output')) {
      if (++i >= args.length) throw new Error(`${arg} requires a filename`);
      output = args[i];
    } else if (!positionalOnly && arg.startsWith('--output=')) {
      output = arg.slice('--output='.length);
    } else if (!positionalOnly && arg.startsWith('-')) {
      throw new Error(`Unknown option: ${arg}`);
    } else {
      inputs.push(arg);
    }
  }
  if (!output) throw new Error('An output image is required (-o IMAGE)');
  if (backup && !update) throw new Error('--backup requires --update');
  const files = inputs.map(input => {
    const separator = input.indexOf(':');
    const source = separator < 0 ? input : input.slice(0, separator);
    const name = separator < 0 ? path.basename(source) : input.slice(separator + 1);
    if (!fs.statSync(source).isFile()) throw new Error(`Not a file: ${source}`);
    return { name, data: fs.readFileSync(source) };
  });
  const existing = update && fs.existsSync(output) ? fs.readFileSync(output) : undefined;
  const image = existing ? updateFat16Image(existing, files) : fat16Image(files);
  if (existing?.equals(image)) {
    console.log(`unchanged ${output}`);
    return;
  }
  fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
  if (existing && backup) {
    let suffix = 0;
    let saved;
    for (;;) {
      saved = `${output}.bak${suffix ? `.${suffix}` : ''}`;
      try {
        fs.copyFileSync(output, saved, fs.constants.COPYFILE_EXCL);
        break;
      } catch (error) {
        if (error.code !== 'EEXIST') throw error;
        ++suffix;
      }
    }
    console.log(`saved previous disk to ${saved}`);
  }
  const temporary = `${output}.tmp-${process.pid}`;
  const descriptor = fs.openSync(temporary, 'wx', existing ? fs.statSync(output).mode : 0o666);
  try {
    try {
      fs.writeFileSync(descriptor, image);
      fs.fsyncSync(descriptor);
    } finally {
      fs.closeSync(descriptor);
    }
    fs.renameSync(temporary, output);
  } finally {
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
  console.log(`wrote ${output} files ${files.length}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    console.error(`error: ${error.message}`);
    process.exitCode = 1;
  }
}
