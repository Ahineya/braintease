// Extract guest-generated files without truncating their 16-bit C cells.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {rootReader} from '../fuzix/tools/read-root.mjs';
import {openImage} from '../fuzix/tools/image.mjs';
const port = path.dirname(fileURLToPath(import.meta.url));
const reader = rootReader(openImage(process.argv[2]));
if(reader.checked.issues.length) throw Error(reader.checked.issues.join('\n'));
for(const name of ['pm.h','onames.h','vis_tab.h','date.h']) {
  const cells=reader.read('/nh/include/'+name);
  fs.writeFileSync(port+'/build/source/include/'+name,Buffer.from(cells));
}
fs.writeFileSync(port+'/build/source/src/vis_tab.c',Buffer.from(reader.read('/nh/src/vis_tab.c')));
if(process.argv.includes('--headers-only')) process.exit(0);
const output = port+'/build/nethack-data';
fs.mkdirSync(output,{recursive:true});
const versionText=fs.readFileSync(port+'/build/source/include/date.h','utf8');
const fingerprints=['NUMBER','FEATURES','SANITY1','SANITY2','SANITY3'].map(key=>{
 const match=versionText.match(new RegExp('#define VERSION_'+key+' (0x[0-9a-f]+)','i'));
 if(!match)throw Error('missing version fingerprint '+key);
 return Number(match[1]);
});
const listing = reader.read('/nh/dat');
const files = [];
for(let i=0;i<listing.length;i+=32) {
  if(!listing[i]) continue;
  const name = String.fromCharCode(...listing.subarray(i+1,i+31)).split('\0')[0];
  if(name==='.'||name==='..') continue;
  const binary = name.endsWith('.lev') || name==='dungeon'||name==='quest.dat';
  const generated = ['bogusmon','data','engrave','epitaph','rumors','oracles','options'];
  const shipped = ['help','hh','keyhelp','wizhelp','cmdhelp','history','license','opthelp','symbols','tribute'];
  if(!binary&&!generated.includes(name)&&!shipped.includes(name)) continue;
  const cells = reader.read('/nh/dat/'+name);
  if(name.endsWith('.lev') || name==='dungeon') {
    for(let j=0;j<fingerprints.length;j++) {
      const actual=(cells[j*2] | cells[j*2+1]<<16)>>>0;
      if(actual!==fingerprints[j])throw Error('incompatible target fingerprint in '+name);
    }
  }
  let data;
  if(binary) {
    data = Buffer.alloc(8+cells.length*2);data.write('NHC16v1\n');
    for(let j=0;j<cells.length;j++) data.writeUInt16LE(cells[j],8+j*2);
  } else {
    if(cells.some(c=>c>255)) throw Error('non-octet text asset '+name);
    data = Buffer.from(cells);
  }
  fs.writeFileSync(output+'/'+name,data); files.push({name,cells:cells.length,binary});
}
if(files.filter(f=>f.name.endsWith('.lev')).length!==120)throw Error('incomplete NetHack 3.6.7 special-level set');
fs.writeFileSync(port+'/build/assets.json',JSON.stringify(files,null,2)+'\n');
console.log('Packed '+files.length+' guest-generated NetHack assets.');
