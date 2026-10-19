// SPDX-License-Identifier: GPL-2.0-only
// Keep the kernel's original notices available alongside its new identity.
import fs from 'node:fs';
export function kernelCredits() {
  const files = [
    ['COPYING', '../upstream/LICENCE'],
    ['CREDITS', '../upstream/CREDITS'],
    ['UPSTREAM.md', '../UPSTREAM.md'],
  ];
  return [
    {name: '/usr/share/doc/riplix/README', mode: 0o100444,
      data: Buffer.from('Riplix is derived from FUZIX.\n' +
        'Original copyright and license notices remain in the sources.\n' +
        'COPYING contains the kernel license; CREDITS preserves upstream credits.\n' +
        'Source and local modifications: https://github.com/Ahineya/braintease\n' +
        'See ports/fuzix/UPSTREAM.md and ports/fuzix/upstream.patch in that tree.\n')},
    ...files.map(([name, source]) => ({name: `/usr/share/doc/riplix/${name}`,
      mode: 0o100444, data: fs.readFileSync(new URL(source, import.meta.url))})),
  ];
}
