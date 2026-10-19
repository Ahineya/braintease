import fs from 'node:fs';
export function klotskiFiles() {
  return ['COPYING','source.json','README.md'].map(source=>({
    name:`/usr/share/doc/klotski/${source}`,mode:0o100644,
    data:fs.readFileSync(new URL(source,import.meta.url))
  }));
}
