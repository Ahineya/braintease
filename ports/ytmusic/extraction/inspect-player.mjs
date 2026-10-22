// SPDX-License-Identifier: MIT
// Count the pinned ES6 fixture's parser objects without executing player code.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const build = path.resolve(process.argv[2] || path.join(here, '../../../build/ytmusic-extraction'));
const pin = JSON.parse(fs.readFileSync(path.join(here, 'UPSTREAM.json'), 'utf8'));
const fixture = pin.players.find(p => p.variant === 'es6');
const data = fs.readFileSync(path.join(build, fixture.filename));
const sha256 = createHash('sha256').update(data).digest('hex');
if (sha256 !== fixture.sha256) throw Error('Player fixture differs from pin');
const parserRoot = path.join(build, 'bundler/node_modules/meriyah-ytjs');
const version = JSON.parse(fs.readFileSync(path.join(parserRoot, 'package.json'), 'utf8')).version;
if (pin.build_dependencies['meriyah-ytjs'] !== 'npm:meriyah@' + version)
    throw Error('Parser version differs from pin');
const { parseScript } = await import(pathToFileURL(path.join(parserRoot, 'dist/meriyah.mjs')));
const pending = [parseScript(data.toString('utf8'), { ranges: true, loc: false, module: false })];
let astNodes = 0, objectsIncludingArrays = 0;
while (pending.length) {
    const item = pending.pop();
    ++objectsIncludingArrays;
    if (typeof item.type === 'string') ++astNodes;
    for (const value of Object.values(item)) {
        if (value && typeof value === 'object') pending.push(value);
    }
}
console.log(JSON.stringify({ player: fixture.id, variant: fixture.variant, sha256,
    sourceBytes: data.length, astNodes, objectsIncludingArrays }, null, 2));
