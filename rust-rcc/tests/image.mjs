// Disposable, minimal images. Never open either normal guest disk.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage} from '../../ports/fuzix/tools/disk.mjs';
import {writeImage} from '../../ports/fuzix/tools/image.mjs';
import {fat16Image} from '../../c-test/examples/rOS/mkfat16.mjs';

const [system, build, app, output, upper, extras] = process.argv.slice(2);
if (!['fuzix', 'fuse-x'].includes(system) || !output) throw Error('image.mjs SYSTEM BUILD APP OUTPUT');
const native = system === 'fuse-x';
const files = native
  ? [{name: '/init', mode: 0o100755, data: Buffer.from('FXN1:init:1\n')},
      ...['sh', 'echo'].map(name => ({name: `/bin/${name}`, mode: 0o100755,
        data: fs.readFileSync(path.join(build, 'apps', name + '.fxe'))}))]
  : ['init', 'sh', 'echo'].map(name => ({name: name === 'init' ? '/init' : `/bin/${name}`, mode: 0o100755,
      data: fs.readFileSync(path.join(build, name + '.rxe'))}));
files.push({name: '/bin/rustprobe', mode: 0o100755, data: fs.readFileSync(app)},
  {name: '/input', mode: 0o100644, data: Buffer.from(Array.from({length: 20009}, (_, i) => i % 256))},
  {name: '/dev/tty', mode: 0o20666, device: 0x200},
  {name: '/dev/console', mode: 0o20600, device: 0x201},
  {name: '/dev/null', mode: 0o20666, device: 0x100});
if (upper) files.push({name: '/bin/upper', mode: 0o100755, data: fs.readFileSync(upper)});
if (extras) for (const [name, entry] of Object.entries(JSON.parse(extras)))
  files.push({name, mode: typeof entry === 'string' ? 0o100755 : entry.mode,
    data: fs.readFileSync(typeof entry === 'string' ? entry : entry.source)});
const root = rootImage(files, 24576, 256);
const image = native ? root : fat16Image([
  {name: 'FUZIX.SYS', data: fs.readFileSync(path.join(build, 'FUZIX.SYS'))},
  {name: 'ROOTFS.IMG', data: root}
], {totalSectors: 131072, sectorsPerFat: 256, sectorsPerCluster: 4});
writeImage(output, image, true);
