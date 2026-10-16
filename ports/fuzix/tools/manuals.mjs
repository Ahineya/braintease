// SPDX-License-Identifier: GPL-2.0-or-later
// Retain upstream manuals as source files; port-specific pages take precedence.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const port = fileURLToPath(new URL('../', import.meta.url));
export function manualFiles() {
  const pages = new Map();
  const add = directory => {
    for (const item of fs.readdirSync(directory, {withFileTypes: true}).sort((a, b) => a.name.localeCompare(b.name))) {
      const filename = path.join(directory, item.name);
      if (item.isDirectory()) add(filename);
      else if (/^[a-z0-9_-]+\.[1-9]$/.test(item.name)) {
        const section = item.name.slice(-1);
        const name = `/usr/man/man${section}/${item.name}`;
        pages.set(name, {name, mode: 0o100644, data: fs.readFileSync(filename)});
      }
    }
  };
  for (const directory of ['upstream/Applications/util', 'upstream/Applications/V7/cmd',
                            'upstream/Applications/V7/games', 'manual'])
    add(path.join(port, directory));
  return [...pages.values()].sort((a, b) => a.name.localeCompare(b.name));
}
