// Extract production sprite initialization and its maintained name order.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const directory = process.argv[2];
if (!directory) throw new Error('usage: sprite-fixture.mjs OUTPUT_DIRECTORY');
const source = fs.readFileSync(path.join(port, 'engine/r_things.c'), 'utf8');
const end = source.indexOf('//\n// GAME FUNCTIONS');
if (end < 0) throw new Error('missing sprite initialization boundary');
const info = fs.readFileSync(path.join(port, 'engine/info.c'), 'utf8');
const names = info.match(/char\s*\*\s*sprnames\s*\[\s*\]\s*=\s*\{[^}]+\}\s*;/)?.[0];
if (!names) throw new Error('missing sprite name initializer');
fs.mkdirSync(directory, { recursive: true });
for (const [name, data] of [['sprite_metadata.inc', source.slice(0, end)], ['sprite_names.inc', names + '\n']]) {
  const file = path.join(directory, name);
  if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== data) fs.writeFileSync(file, data);
}
