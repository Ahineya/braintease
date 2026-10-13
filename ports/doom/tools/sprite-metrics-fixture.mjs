// Extract the maintained sprite metric loader plus its unchanged raw fallback.
import fs from 'node:fs';
import path from 'node:path';
const directory = process.argv[2];
if (!directory) throw new Error('usage: sprite-metrics-fixture.mjs OUTPUT_DIRECTORY');
const source = fs.readFileSync(new URL('../engine/r_data.c', import.meta.url), 'utf8');
const marker = '#if defined(RIPPLE) && DOOM_WAD_MODE == 2\nstatic int R_LoadCookedSpriteMetrics(void)';
const start = source.indexOf(marker), end = source.indexOf('//\n// R_InitColormaps', start);
if (start < 0 || end < 0) throw new Error('missing sprite metric loader boundaries');
fs.mkdirSync(directory, { recursive: true });
const file = path.join(directory, 'sprite_metrics.inc'), data = source.slice(start, end);
if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8') !== data) fs.writeFileSync(file, data);
