import fs from 'node:fs';
export function shikakuFiles() {
  return ['README.md'].map(source=>({name:`/usr/share/doc/shikaku/${source}`,
    mode:0o100644,data:fs.readFileSync(new URL(source,import.meta.url))}));
}
