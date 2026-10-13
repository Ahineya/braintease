// Compile the production storage declarations and initializers without the
// unrelated renderer. Changes to their bodies are exercised by the fixture.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const directory = process.argv[2];
if (!directory) throw new Error('usage: renderer-storage-fixture.mjs OUTPUT_DIRECTORY');
const main = fs.readFileSync(path.join(port, 'engine/r_main.c'), 'utf8');
const plane = fs.readFileSync(path.join(port, 'engine/r_plane.c'), 'utf8');
const globalsEnd = main.indexOf('//\n// R_AddPointToBox');
const storageStart = main.indexOf('static size_t renderer_storage_bytes;');
const storageEnd = main.indexOf('void R_Init (void)', storageStart);
const planeEnd = plane.indexOf('//\n// R_MapPlane');
if (globalsEnd < 0 || storageStart < globalsEnd || storageEnd < storageStart || planeEnd < 0)
  throw new Error('missing renderer storage extraction boundary');
const data = main.slice(0, globalsEnd) + '\n' + main.slice(storageStart, storageEnd)
  + '\n' + plane.slice(0, planeEnd);
fs.mkdirSync(directory, { recursive: true });
const file = path.join(directory, 'renderer_storage.inc');
if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== data) fs.writeFileSync(file, data);
