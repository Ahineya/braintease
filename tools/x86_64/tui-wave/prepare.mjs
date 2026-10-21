// Install into a private sparse clone. Never open a device or the HP filesystem.
import fs from 'node:fs';
import {openImage, writeImage} from '../../../ports/fuzix/tools/image.mjs';
import {installRoot} from '../../../ports/fuzix/tools/install.mjs';
import {commanderFiles, riftFiles} from '../../../rfl/os/files.mjs';
const [seed, output, apps, catalogPath, mode] = process.argv.slice(2);
const files = catalogPath ? [] : [...commanderFiles(), ...riftFiles()];
for (const name of ['mux', 'rfl-fm', 'rift', 'keycheck', 'muxcheck', 'levee-fixed']) {
  if (catalogPath && !['muxcheck', 'levee-fixed'].includes(name)) continue;
  const path = `${apps}/${name}.fxe`;
  if (fs.existsSync(path)) files.push({name: `/bin/${name}`, mode: 0o100755, data: fs.readFileSync(path)});
}
if (catalogPath) {
  files.push({name: '/bin/pkg-stage', mode: 0o100755,
    data: fs.readFileSync(new URL('../../../build/pkg/pkg-engine-x64.rxe', import.meta.url))});
  if (mode === 'managed') {
    function collect(directory, prefix='') {
      for (const entry of fs.readdirSync(directory, {withFileTypes:true})) {
        const path = `${directory}/${entry.name}`, name = `${prefix}/${entry.name}`;
        if (entry.isDirectory()) collect(path, name);
        else if (entry.isFile()) files.push({name, mode: 0o100000 | (fs.statSync(path).mode & 0o777), data: fs.readFileSync(path)});
        else if (entry.isSymbolicLink()) {
          if (!fs.realpathSync(path).startsWith(fs.realpathSync(catalogPath) + '/')) throw Error('managed symlink escapes private root');
          const target = fs.readlinkSync(path);
          if (target.startsWith('/')) throw Error('managed symlink must be relative');
          files.push({name, symlink: target});
        }
        else throw Error('managed fixture contains a non-regular path');
      }
    }
    collect(catalogPath);
  } else {
    const catalog = JSON.parse(fs.readFileSync(catalogPath));
    catalog.packages.forEach((p, index) => files.push({name: `/tmp/tui${index}.rpk`, mode: 0o100600,
      data: fs.readFileSync(p.archive)}));
  }
}
writeImage(output, installRoot(openImage(seed), files), true);
