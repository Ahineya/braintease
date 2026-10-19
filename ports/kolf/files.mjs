import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
export function kolfFiles() {
  return [
    ...['Practice','ReallyEasy','Easy','Medium','Hard','Impossible','Classic','USApro'].map(name=>[`courses/${name}.kolf`,`/usr/share/kolf/courses/${name}.kolf`]),
    ['COPYING','/usr/share/doc/kolf/COPYING'],
    ['source.json','/usr/share/doc/kolf/source.json'],
  ].map(([source,name])=>({name,mode:0o100644,
    data:fs.readFileSync(fileURLToPath(new URL(source,import.meta.url)))}));
}
