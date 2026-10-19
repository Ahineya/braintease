// Called by install.py with both images locked. Secret bytes stay in memory.
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {openImage, writeImage} from '../fuzix/tools/image.mjs';
import {fatFile} from '../fuzix/tools/fat.mjs';
import {checkRoot} from '../fuzix/tools/fsck.mjs';
import {installRoot} from '../fuzix/tools/install.mjs';
import {atomicWrite} from '../fuzix/tools/disk.mjs';
import {CELLS, BLOCK} from '../fuzix/tools/layout.mjs';

function readFile(checked, pathname, required = true) {
  let node = checked.nodes.get(1);
  function cell(at) {
    const block = node.map.get(Math.floor(at / CELLS));
    return block ? checked.image.readUInt16LE(block * BLOCK + (at % CELLS) * 2) : 0;
  }
  for (const part of pathname.split('/').filter(Boolean)) {
    if (node.type !== 0x4000) throw Error('login path contains a non-directory');
    let child;
    for (let at = 0; at < node.size; at += 32) {
      if (!cell(at)) continue;
      let name = '';
      for (let j = 1; j <= 30 && cell(at + j); j++) name += String.fromCharCode(cell(at + j));
      if (name === part) { child = checked.nodes.get(cell(at)); break; }
    }
    if (!child) {
      if (required) throw Error(`missing private login file: ${pathname}`);
      return null;
    }
    node = child;
  }
  const mode = checked.image.readUInt16LE(node.base * 2);
  if (node.type !== 0x8000 || mode & 0o077 || node.links !== 1 || node.size > 2048)
    throw Error(`login file must be a private regular file with one link: ${pathname}`);
  const data = Buffer.alloc(node.size);
  for (let at = 0; at < node.size; at++) {
    const value = cell(at);
    if (value > 255) throw Error(`invalid octet in login file: ${pathname}`);
    data[at] = value;
  }
  return data;
}

try {
  const [target, source, destination, executable, ...rest] = process.argv.slice(2);
  if (rest.length || !['native', 'ripple'].includes(target) || !executable)
    throw Error('use ports/telegram/install.py with stopped source and destination images');
  const saved = checkRoot(openImage(source));
  if (saved.issues.length) throw Error('source filesystem needs a clean shutdown or offline repair');
  const config = readFile(saved, '/etc/telegram.conf'), session = readFile(saved, '/telegram.session');
  const lines = config.toString('ascii').split(/\r?\n/).filter(line => line && !line.startsWith('#'));
  const ids = lines.filter(line => /^api_id=[0-9]+$/.test(line));
  const hashes = lines.filter(line => /^api_hash=[a-fA-F0-9]{32}$/.test(line));
  const id = Number(ids[0]?.slice(7));
  if (config.some(byte => byte > 127) || lines.length !== 2 || ids.length !== 1 || hashes.length !== 1 || !id || id > 2147483647)
    throw Error('invalid private application configuration');
  if (session.length !== 328 || session.toString('ascii', 0, 4) !== 'FTG1' ||
      session.readUInt32LE(4) !== id || session[8] < 1 || session[8] > 5 ||
      session[9] !== 0 || session[10] !== 1 || session[11] ||
      session.subarray(288, 296).some(Boolean) ||
      !createHash('sha256').update(session.subarray(0, 296)).digest().equals(session.subarray(296)))
    throw Error('invalid or mismatched saved production login');
  const original = openImage(destination);
  const root = target === 'native' ? {data: original, replace: value => value} : fatFile(original, 'ROOTFS.IMG');
  const checked = checkRoot(root.data, true);
  if (checkRoot(checked.image).issues.length) throw Error('destination filesystem repair failed');
  const files = [
    {name:'/bin/telegram', mode:0o100755, data:fs.readFileSync(executable)},
    {name:'/etc/telegram.conf', mode:0o100600, data:config},
    {name:'/telegram.session', mode:0o100600, data:session},
  ];
  for (const file of files.slice(1)) {
    const previous = readFile(checked, file.name, false);
    if (previous && !previous.equals(file.data))
      throw Error(`destination has a different login; preserving ${file.name}`);
  }
  const installed = installRoot(checked.image, files);
  const verified = checkRoot(installed);
  if (verified.issues.length || !readFile(verified, '/telegram.session').equals(session) ||
      !readFile(verified, '/etc/telegram.conf').equals(config)) throw Error('installed login verification failed');
  const updated = root.replace(installed);
  fs.chmodSync(destination, 0o600);
  if (original.equals(updated)) {
    console.log('Telegram and saved login are already installed.');
  } else {
    let backup = destination + '.bak', serial = 0;
    while (fs.existsSync(backup)) backup = destination + '.bak.' + ++serial;
    writeImage(backup, original, true);fs.chmodSync(backup, 0o600);
    atomicWrite(destination, updated);fs.chmodSync(destination, 0o600);
    console.log(`Installed Telegram and private saved login in ${destination}; backup: ${backup}`);
  }
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
