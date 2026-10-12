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

export function fat16Image(files) {
  const rootSecs = Math.ceil(ROOT_ENT * 32 / BPS);
  const fatOffset = RESERVED * BPS;
  const rootOffset = (RESERVED + NUM_FATS * SECS_PER_FAT) * BPS;
  const dataOffset = rootOffset + rootSecs * BPS;
  const clusterBytes = SPC * BPS;
  const clusterCount = Math.floor((TOTAL_SECS * BPS - dataOffset) / clusterBytes);
  if (files.length > ROOT_ENT) throw new Error(`FAT root directory holds at most ${ROOT_ENT} files`);
  const requiredClusters = files.reduce((total, file) => total + Math.ceil(file.data.length / clusterBytes), 0);
  if (requiredClusters > clusterCount) throw new Error('Files exceed FAT16 image capacity');

  const image = Buffer.alloc(TOTAL_SECS * BPS);
  image.set([0xeb, 0x3c, 0x90], 0);
  image.write('MSDOS5.0', 3, 'ascii');
  image.writeUInt16LE(BPS, 11);
  image[13] = SPC;
  image.writeUInt16LE(RESERVED, 14);
  image[16] = NUM_FATS;
  image.writeUInt16LE(ROOT_ENT, 17);
  image.writeUInt16LE(TOTAL_SECS, 19);
  image[21] = MEDIA;
  image.writeUInt16LE(SECS_PER_FAT, 22);
  image.writeUInt16LE(32, 24);
  image.writeUInt16LE(2, 26);
  // Preserve the existing image layout, including its total-sector mirror.
  image.writeUInt32LE(TOTAL_SECS, 32);
  image[36] = 0x80;
  image[38] = 0x29;
  image.writeUInt32LE(0x12345678, 39);
  image.write('RIPPLEROS  ', 43, 'ascii');
  image.write('FAT16   ', 54, 'ascii');
  image.writeUInt16LE(0xaa55, 510);

  function fatSet(cluster, value) {
    for (let fat = 0; fat < NUM_FATS; ++fat) {
      image.writeUInt16LE(value, fatOffset + fat * SECS_PER_FAT * BPS + cluster * 2);
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
    image[entry + 11] = 0x20;
    image.writeUInt16LE(first, entry + 26);
    image.writeUInt32LE(data.length, entry + 28);
  }
  return image;
}

function main(args) {
  let output;
  const inputs = [];
  let positionalOnly = false;
  for (let i = 0; i < args.length; ++i) {
    const arg = args[i];
    if (!positionalOnly && arg === '--') {
      positionalOnly = true;
    } else if (!positionalOnly && (arg === '-h' || arg === '--help')) {
      console.log('Usage: node mkfat16.mjs -o IMAGE [src:DESTNAME | src ...]');
      return;
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
  const files = inputs.map(input => {
    const separator = input.indexOf(':');
    const source = separator < 0 ? input : input.slice(0, separator);
    const name = separator < 0 ? path.basename(source) : input.slice(separator + 1);
    if (!fs.statSync(source).isFile()) throw new Error(`Not a file: ${source}`);
    return { name, data: fs.readFileSync(source) };
  });
  const image = fat16Image(files);
  fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
  fs.writeFileSync(output, image);
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
