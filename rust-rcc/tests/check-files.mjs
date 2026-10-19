// Verify the on-disk bytes independently after the Rust application exits.
import {openImage} from '../../ports/fuzix/tools/image.mjs';
import {fatFile} from '../../ports/fuzix/tools/fat.mjs';
import {rootReader} from '../../ports/fuzix/tools/read-root.mjs';
const [system, filename] = process.argv.slice(2);
const image = openImage(filename);
const root = system === 'fuzix' ? fatFile(image, 'ROOTFS.IMG').data : image;
const reader = rootReader(root);
const input = reader.read('/input');
for (const path of ['/output', '/second']) {
  const output = reader.read(path);
  if (input.length !== 20009 || output.length !== input.length) throw Error('unexpected file size');
  for (let i = 0; i < input.length; i++) {
    const before = i % 256, after = before >= 97 && before <= 122 ? before - 32 : before;
    if (input[i] !== before || output[i] !== after) throw Error(`${path}: file content mismatch at ${i}`);
  }
}
let removed = false;
try { reader.lookup('/scratch'); } catch (error) { if (String(error).includes('missing /scratch')) removed = true; else throw error; }
if (!removed) throw Error('Rust remove_file left /scratch behind');
console.log(`${system}: Rust utility output verified on disk (including NUL/non-UTF-8 bytes)`);
