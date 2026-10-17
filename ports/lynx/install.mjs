// Invoked by install.py with both host image locks held.
import fs from 'node:fs';
import {openImage, writeImage} from '../fuzix/tools/image.mjs';
import {fatFile} from '../fuzix/tools/fat.mjs';
import {installRoot} from '../fuzix/tools/install.mjs';
import {atomicWrite} from '../fuzix/tools/disk.mjs';
import {lynxFiles} from './files.mjs';

const [target, filename, executable] = process.argv.slice(2);
if (!['native', 'ripple'].includes(target) || !filename || !executable)
  throw Error('use ports/lynx/install.py --target TARGET --disk IMAGE');
const original = openImage(filename);
const root = target === 'native' ? {data: original, replace: data => data} : fatFile(original, 'ROOTFS.IMG');
const files = [
  {name:'/bin/lynx', mode:0o100755, data:fs.readFileSync(executable)},
  ...lynxFiles(),
];
const updated = root.replace(installRoot(root.data, files));
if (original.equals(updated)) {
  console.log('Lynx is already installed; configuration preserved.');
} else {
  let backup = filename + '.bak', serial = 0;
  while (fs.existsSync(backup)) backup = filename + '.bak.' + ++serial;
  writeImage(backup, original, true);
  atomicWrite(filename, updated);
  console.log(`Installed /bin/lynx; configuration preserved. Previous disk: ${backup}`);
}
