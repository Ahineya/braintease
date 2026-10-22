import fs from 'node:fs';
import {openImage,writeImage} from '../../../ports/fuzix/tools/image.mjs';
import {growRoot,installRoot} from '../../../ports/fuzix/tools/install.mjs';
const [seed,output,apps]=process.argv.slice(2);
const root=growRoot(openImage(seed),65536,1024);
const files=['lvcalc','screenshot'].map(name=>({name:'/bin/'+name,mode:0o100755,data:fs.readFileSync(apps+'/'+name+'.fxe')}));
files.push({name:'/dev/gfx',mode:0o20666,device:0x400});
writeImage(output,installRoot(root,files),true);
