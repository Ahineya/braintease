import fs from 'node:fs';

// Keep embedded font/data notices available in installed guest systems.
export function unicodeFiles() {
  return ['FONT-COPYRIGHT.txt', 'FONT-LICENSE.txt', 'UNICODE-LICENSE.txt'].map(name => ({
    name: `/usr/share/licenses/unicode/${name}`,
    mode: 0o100644,
    data: fs.readFileSync(new URL(name, import.meta.url))
  }));
}
