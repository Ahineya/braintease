import fs from 'node:fs';
import {compilerFiles} from './files.mjs';
import {openImage, writeImage} from '../ports/fuzix/tools/image.mjs';
import {fatFile} from '../ports/fuzix/tools/fat.mjs';
import {checkRoot} from '../ports/fuzix/tools/fsck.mjs';
import {installRoot, growRoot} from '../ports/fuzix/tools/install.mjs';
import {atomicWrite} from '../ports/fuzix/tools/disk.mjs';
import {ROOT_BLOCKS} from '../ports/fuzix/tools/layout.mjs';
const [osname, filename, directory] = process.argv.slice(2);
try {
  if (!['fuzix', 'fuse-x'].includes(osname) || !filename || !directory) throw Error('expected OS IMAGE PACKAGE');
  const original = openImage(filename);
  const container = osname === 'fuzix' ? fatFile(original, 'ROOTFS.IMG') : null;
  const root = container ? container.data : original;
  const checked = checkRoot(root, true);
  const files = compilerFiles(directory);
  if (!files.length) throw Error('compiler package is empty');
  const updated = installRoot(growRoot(checked.image, ROOT_BLOCKS, 2048), files);
  if (checkRoot(updated).issues.length) throw Error('compiler installation failed filesystem verification');
  const result = container ? container.replace(updated) : updated;
  if (original.equals(result)) console.log('Compiler already installed: '+filename);
  else {
    let backup = filename+'.bak', n = 1;
    while (fs.existsSync(backup)) backup = filename+'.bak.'+(n++);
    writeImage(backup, original, true);
    atomicWrite(filename, result);
    console.log('Installed compiler and SDK: '+filename+'; backup: '+backup);
  }
} catch (error) {console.error(error.message); process.exitCode = 1;}
