// Read the result through the filesystem parser, independently of guest Rust.
import fs from 'node:fs';
import {openImage} from '../../ports/fuzix/tools/image.mjs';
import {fatFile} from '../../ports/fuzix/tools/fat.mjs';
import {rootReader} from '../../ports/fuzix/tools/read-root.mjs';
const [system, filename, expected, resultPath = '/crate.out'] = process.argv.slice(2);
const image = openImage(filename);
const root = system === 'fuzix' ? fatFile(image, 'ROOTFS.IMG').data : image;
const bytes = Buffer.from(rootReader(root).read(resultPath));
if (!bytes.equals(fs.readFileSync(expected))) throw Error('crate output differs from native Rust');
console.log(`${system}: all ${bytes.length} output bytes match native Rust`);
