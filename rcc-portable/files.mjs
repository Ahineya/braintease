// Managed compiler files shared by normal images and the standalone installer.
import fs from 'node:fs';
import path from 'node:path';
export function compilerFiles(directory) {
  const manifest = path.join(directory, 'manifest.json');
  if (!fs.existsSync(manifest)) return [];
  return JSON.parse(fs.readFileSync(manifest, 'utf8')).map(entry => {
    if (!entry.name.startsWith('/bin/') && !entry.name.startsWith('/usr/lib/rcc/') && !entry.name.startsWith('/usr/src/rcc/'))
      throw Error('invalid compiler install path');
    const source = path.resolve(directory, entry.file);
    if (!source.startsWith(path.resolve(directory) + path.sep)) throw Error('invalid compiler package file');
    return {name: entry.name, mode: entry.mode, data: fs.readFileSync(source)};
  });
}
