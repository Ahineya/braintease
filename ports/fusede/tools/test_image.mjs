// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal disposable RVM integration image. No update of a user's disk.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage} from '../../fuzix/tools/disk.mjs';
import {fat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {fuseDEFiles} from '../files.mjs';
const [build, output, probeName, probeFile] = process.argv.slice(2);
if(!build || !output)throw Error('usage: test_image.mjs BUILD OUTPUT');
if(Boolean(probeName)!==Boolean(probeFile))throw Error('probe name and executable must be supplied together');
const names=['init','sh','echo','cat','kill','sleep','nano-X','nanowm','nxcheck','fdeshowcase','nxterm','top','fdectl'];
const files=names.map(name=>({name:name==='init'?'/init':`/bin/${name}`,mode:0o100755,
    data:fs.readFileSync(path.join(build,`${name}.rxe`))}));
if(probeName && probeFile)files.push({name:`/bin/${probeName}`,mode:0o100755,data:fs.readFileSync(probeFile)});
files.push(...fuseDEFiles(path.join(build,'fusede')));
files.push({name:'/dev/tty',mode:0o20666,device:0x200},
    {name:'/dev/console',mode:0o20600,device:0x201},
    {name:'/dev/gfx',mode:0o20600,device:0x400},
    {name:'/dev/proc',mode:0o20444,device:0x103},
    {name:'/dev/null',mode:0o20666,device:0x100});
for(let i=0;i<8;i++)files.push(
    {name:`/dev/ptyp${i}`,mode:0o20600,device:0x300+i},
    {name:`/dev/ttyp${i}`,mode:0o20600,device:0x202+i});
const root=rootImage(files,32768,256);
fs.writeFileSync(output,fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(path.join(build,'FUZIX.SYS'))},
    {name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
