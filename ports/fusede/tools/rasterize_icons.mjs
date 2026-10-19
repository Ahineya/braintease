// SPDX-License-Identifier: GPL-2.0-or-later
// Maintainer tool only. Ordinary builds use the checked-in masks, without sharp.
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const require = createRequire(import.meta.url);
const sharp = require(process.env.FDE_SHARP_MODULE || 'sharp');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const input = path.join(root, 'icons/lucide');
const manifest = JSON.parse(fs.readFileSync(path.join(input, 'UPSTREAM.json')));
const result = {version:1, revision:manifest.revision, sizes:[16,24,32],
    rasterizer:{sharp:sharp.versions.sharp, vips:sharp.versions.vips, rsvg:sharp.versions.rsvg,
                supersampling:4, alpha_threshold:128}, icons:{}};
for (const file of manifest.files) {
    const source = fs.readFileSync(path.join(input, file.file));
    if (createHash('sha256').update(source).digest('hex') !== file.sha256)
        throw Error(`Source hash mismatch: ${file.file}`);
    if (!file.file.endsWith('.svg')) continue;
    const name = file.file.slice(0,-4);
    result.icons[name] = {};
    for (const size of result.sizes) {
        const {data, info} = await sharp(Buffer.from(source.toString().replaceAll('currentColor','#ffffff')),
            {density:72*size/24*4}).resize(size,size).ensureAlpha().raw().toBuffer({resolveWithObject:true});
        if(info.width!==size || info.height!==size || info.channels!==4)throw Error('unexpected raster format');
        const words=[];
        for(let y=0;y<size;y++)for(let base=0;base<size;base+=16) {
            let word=0;
            for(let bit=0;bit<Math.min(16,size-base);bit++)
                if(data[(y*size+base+bit)*4+3]>=128)word|=0x8000>>>bit;
            words.push(word);
        }
        result.icons[name][size]=words;
    }
}
const output=path.join(root,'icons/masks.json');
fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');
console.log(`Rasterized ${Object.keys(result.icons).length} Lucide icons at ${result.sizes.join('/')} pixels`);
