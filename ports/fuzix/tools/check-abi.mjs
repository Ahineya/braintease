#!/usr/bin/env node
// Cheap stopped-disk preflight; full consistency checking belongs to fsck.
import {openImage} from './image.mjs';
import {fatFile} from './fat.mjs';
import {layout} from './layout.mjs';
try {
  if (process.argv.length !== 3) throw Error('usage: check-abi.mjs IMAGE');
  const root = fatFile(openImage(process.argv[2]), 'ROOTFS.IMG').data;
  if (layout(root).version !== 3)
    throw Error('C16 V2 disk needs an upgrade; run without --run-only, or use --install-apps with --disk. The previous disk will be backed up.');
} catch (error) {
  console.error(`Disk format check failed: ${error.message}`);
  process.exitCode = 1;
}
