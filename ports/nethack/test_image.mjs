// Disposable test disks only; never reads a normal user image.
import fs from 'node:fs';
import {rootImage} from '../fuzix/tools/disk.mjs';
import {Image,writeImage} from '../fuzix/tools/image.mjs';
import {fat16Image} from '../../c-test/examples/rOS/mkfat16.mjs';
const [manifest,disk,target,build] = process.argv.slice(2);
const native = target === 'native';
const files = JSON.parse(fs.readFileSync(manifest)).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.from(f.text||'')}));
for (const name of ['sh','echo','cat']) files.push({name:'/bin/'+name,mode:0o100755,data:fs.readFileSync(build+(native?'/apps/'+name+'.fxe':'/'+name+'.rxe'))});
if (native && fs.existsSync(build+'/lib')) for (const name of fs.readdirSync(build+'/lib').filter(n=>/\.fxl\.\d+$/.test(n))) files.push({name:'/lib/fuse-x/'+name,data:fs.readFileSync(build+'/lib/'+name)});
files.push({name:'/init',mode:0o100755,data:native?Buffer.from('FXN1:init:1\n'):fs.readFileSync(build+'/init.rxe')},
{name:'/dev/tty',mode:0o20666,device:0x200},{name:'/dev/console',mode:0o20600,device:0x201},
{name:'/dev/null',mode:0o20666,device:0x100},{name:'/dev/urandom',mode:0o20444,device:0x104});
const root = rootImage(files,native?65536:196608,1024);
writeImage(disk,native?root:fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:root}],{totalSectors:1048576,sectorsPerFat:256,sectorsPerCluster:16,imageFactory:size=>new Image(size)}));
