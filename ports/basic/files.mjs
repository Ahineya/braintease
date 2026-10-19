import fs from 'node:fs';
export function basicFiles() {
  return ['hello.bas', 'granny.bas'].map(name => ({
    name: `/usr/share/basic/${name}`, mode: 0o100644,
    data: fs.readFileSync(new URL(`examples/${name}`, import.meta.url))
  }));
}
