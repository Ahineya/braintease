// SPDX-License-Identifier: GPL-2.0-only
// Raw, tiny C16 V3 root; no outer FAT disk, packages or BIOS kernel loader.
import {rootImage, atomicWrite} from '../tools/disk.mjs';
import {installRoot} from '../tools/install.mjs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
export function bootDisk(filename, executable) {
  const programs = executable ? ['cat', 'ls', 'uname'].map(name => ({
    name: `/bin/${name}`, mode: 0o100555,
    data: fs.readFileSync(path.join(path.dirname(executable), 'bin', `${name}.sys`))
  })) : [];
  const root = rootImage([
    {name: '/BOOT', mode: 0o100444, data: Buffer.from('FUZIX kernel running in Brainfuck.\n')},
    ...(executable ? [{name: '/init', mode: 0o100555, data: fs.readFileSync(executable)}] : []),
    ...programs
  ], executable ? 1024 : 128, 32);
  atomicWrite(filename, installRoot(root, [{
    name: '/README', mode: 0o100444,
    data: Buffer.from('FUZIX running on the Brainfuck CPU.\n' +
      (executable ? 'Type help for commands. Use shutdown to exit.\n' +
        'Files are reset when you start a new session.\n' :
        'Read-only kernel boot check.\n'))
  }]));
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv.length < 3 || process.argv.length > 4) throw Error('usage: disk.mjs OUTPUT [INIT.sys]');
  bootDisk(process.argv[2], process.argv[3]);
}
