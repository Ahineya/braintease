// Bounded recovery copies for guest disks. Callers own the guest image lock.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {openImage, writeImage} from './image.mjs';

export function backupLimit(value = process.env.RIPPLE_IMAGE_BACKUPS ?? '1') {
  if (!/^(0|[1-9][0-9]?)$/.test(String(value)))
    throw Error('RIPPLE_IMAGE_BACKUPS must be an integer from 0 to 99');
  return Number(value);
}

export function imageBackups(filename) {
  const directory = path.dirname(filename), name = path.basename(filename) + '.bak';
  return fs.readdirSync(directory).filter(n => n === name || n.startsWith(name + '.'))
    .map(n => path.join(directory, n)).filter(p => {
      const info = fs.lstatSync(p);
      return info.isFile(); // Never follow a link supplied by another directory entry.
    });
}

export function backupImage(filename, image, {keep = backupLimit()} = {}) {
  backupLimit(keep);
  if (!keep) return null;
  const saved = filename + '.bak';
  const temporary = fs.mkdtempSync(path.join(path.dirname(filename), '.image-backup-'));
  try {
    const staged = path.join(temporary, 'image');
    // Sparse/clone-aware writer; a backup must not materialize a 1–2 GiB hole.
    writeImage(staged, image, true);
    if (keep > 1 && fs.existsSync(saved)) {
      let serial = 1;
      while (fs.existsSync(saved + '.' + serial)) ++serial;
      fs.renameSync(saved, saved + '.' + serial);
    }
    fs.renameSync(staged, saved);
    const fd = fs.openSync(path.dirname(filename), 'r');
    try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
    return saved;
  } finally {
    fs.rmSync(temporary, {recursive: true, force: true});
  }
}

export function pruneImageBackups(filename, {keep = backupLimit()} = {}) {
  backupLimit(keep);
  const saved = filename + '.bak';
  const backups = imageBackups(filename).sort((a, b) => {
    if (a === saved) return -1;
    if (b === saved) return 1;
    return fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs || b.localeCompare(a);
  });
  // Only prune after the replacement image has been committed successfully.
  const removed = backups.slice(keep);
  for (const file of removed) fs.unlinkSync(file);
  return removed;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [command, filename, ...extra] = process.argv.slice(2);
    if (!filename || extra.length || !['save', 'prune'].includes(command))
      throw Error('usage: image-backup.mjs save|prune IMAGE');
    if (command === 'save') {
      const saved = backupImage(filename, openImage(filename));
      console.log(saved ? `Previous image saved to ${saved}` : 'Image backups disabled');
    } else pruneImageBackups(filename);
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
