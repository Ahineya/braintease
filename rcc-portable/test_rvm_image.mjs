// Private-disk support for test_rvm.py; never opens the user's FUZIX disk.
import fs from 'node:fs';
import {fatFile} from '../ports/fuzix/tools/fat.mjs';
import {openImage} from '../ports/fuzix/tools/image.mjs';
import {atomicWrite, buildDisk} from '../ports/fuzix/tools/disk.mjs';
import {installRoot} from '../ports/fuzix/tools/install.mjs';
import {checkRoot} from '../ports/fuzix/tools/fsck.mjs';
import {BLOCK, CELLS} from '../ports/fuzix/tools/layout.mjs';

const [action, image, directory, manifest] = process.argv.slice(2);
if (action === 'create' || action === 'install-native') {
  if (action === 'create') buildDisk(directory, image, true);
  const original = openImage(image);
  const container = action === 'create' ? fatFile(original, 'ROOTFS.IMG') : null;
  const root = container ? container.data : original;
  const files = JSON.parse(fs.readFileSync(manifest)).map(file => ({
    name: file.guest, mode: file.mode ?? 0o100644, data: fs.readFileSync(file.host)
  }));
  const updated = installRoot(root, files);
  atomicWrite(image, container ? container.replace(updated) : updated);
} else if (action === 'extract') {
  const root = fatFile(openImage(image), 'ROOTFS.IMG').data;
  const checked = checkRoot(root);
  if (checked.issues.length) throw Error(JSON.stringify(checked.issues));
  function cells(node) {
    const result = new Uint16Array(node.size);
    for (const [index, block] of node.map) {
      const data = root.subarray(block * BLOCK, (block + 1) * BLOCK);
      for (let j = 0; j < CELLS && index * CELLS + j < node.size; j++)
        result[index * CELLS + j] = data.readUInt16LE(j * 2);
    }
    return result;
  }
  for (const file of JSON.parse(fs.readFileSync(manifest))) {
    let node = checked.nodes.get(1);
    for (const part of file.guest.split('/').filter(Boolean)) {
      const entries = cells(node);
      let next;
      for (let i = 0; i < entries.length; i += 32) {
        let name = '';
        for (let j = 1; j <= 30 && entries[i + j]; j++) name += String.fromCharCode(entries[i + j]);
        if (entries[i] && name === part) next = checked.nodes.get(entries[i]);
      }
      if (!next) throw Error(`missing guest file ${file.guest}`);
      node = next;
    }
    const text = cells(node);
    if (text.some(cell => cell > 255)) throw Error(`non-octet IR in ${file.guest}`);
    fs.writeFileSync(file.host, Buffer.from(text));
  }
} else {
  throw Error('expected create, install-native or extract');
}
