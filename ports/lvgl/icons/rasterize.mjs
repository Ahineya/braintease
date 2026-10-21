// Maintainer-only SVG authoring. Ordinary builds consume checked-in A8 tables.
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const require = createRequire(import.meta.url);
const sharp = require(process.env.LVGL_SHARP_MODULE || 'sharp');
const here = path.dirname(fileURLToPath(import.meta.url));
const source = path.resolve(here, '../../fusede/icons/lucide');
const upstream = JSON.parse(fs.readFileSync(path.join(source, 'UPSTREAM.json')));
const entries = [];
for (const file of upstream.files) {
    const data = fs.readFileSync(path.join(source, file.file));
    if (createHash('sha256').update(data).digest('hex') !== file.sha256)
        throw Error(`Changed pinned source: ${file.file}`);
    if (file.file.endsWith('.svg')) entries.push({name:file.file.slice(0,-4), file:file.file, turns:0});
}
entries.push({name:'arrow-up', file:'arrow-left.svg', turns:1});
entries.sort((a,b)=>a.name.localeCompare(b.name,'en'));
const result = {revision:upstream.revision, sizes:[16,32],
    rasterizer:{sharp:sharp.versions.sharp,vips:sharp.versions.vips,rsvg:sharp.versions.rsvg,supersampling:4}, icons:[]};
for (const entry of entries) {
    const svg = fs.readFileSync(path.join(source,entry.file),'utf8').replaceAll('currentColor','#ffffff');
    const item = {...entry, masks:{}};
    for (const size of result.sizes) {
        // Render at 4x and downsample coverage, without binary thresholding.
        let pipeline = sharp(Buffer.from(svg),{density:72*size/24*4}).resize(size,size);
        if(entry.turns) pipeline=pipeline.rotate(90*entry.turns);
        const {data,info}=await pipeline.ensureAlpha().raw().toBuffer({resolveWithObject:true});
        if(info.width!==size || info.height!==size || info.channels!==4)throw Error('Unexpected raster');
        item.masks[size]=Array.from({length:size*size},(_,i)=>data[i*4+3]);
    }
    result.icons.push(item);
}
fs.writeFileSync(path.join(here,'masks.json'),JSON.stringify(result)+'\n');
console.log(`Baked ${entries.length} icons at 16/32 pixels`);
