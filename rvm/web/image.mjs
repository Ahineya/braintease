// The browser seed has no input-image argument. Only this source allowlist is read.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {rootImage} from '../../ports/fuzix/tools/disk.mjs';
import {Image,writeImage} from '../../ports/fuzix/tools/image.mjs';
import {fat16Image} from '../../c-test/examples/rOS/mkfat16.mjs';
import {ptyDevices} from '../../ports/fuzix/tools/pty.mjs';
import {kernelCredits} from '../../ports/fuzix/tools/credits.mjs';
import {manualFiles} from '../../ports/fuzix/tools/manuals.mjs';
import {sokobanFiles} from '../../ports/fuzix/tools/sokoban.mjs';
import {basicFiles} from '../../ports/basic/files.mjs';
import {commanderFiles,riftFiles} from '../../rfl/os/files.mjs';
import {unicodeFiles} from '../../libraries/unicode/files.mjs';
import {applications,aliases} from './apps.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const [output,kernelDirectory=path.join(root,'build/rvm-web/fuzix')]=process.argv.slice(2);
if(!output||process.argv.length>4)throw Error('usage: image.mjs OUTPUT [KERNEL_BUILD] (always creates a fresh image)');
const manifest=applications.map(name=>({name:name==='init'?'/init':'/bin/'+name,mode:0o100755,source:'build/fuzix/'+name+'.rxe'}));
const files=manifest.map(f=>{const data=fs.readFileSync(path.join(root,f.source));if(data.toString('ascii',0,4)!=='RXE1')throw Error('not a guest executable: '+f.source);return {...f,data};});
const resources=[...manualFiles(),...sokobanFiles(),...basicFiles(),...commanderFiles(),...riftFiles(),...unicodeFiles(),...kernelCredits()];
files.push(...Object.entries(aliases).map(([name,symlink])=>({name:'/bin/'+name,symlink})),
 ...resources,...ptyDevices(),
 {name:'/dev/tty',mode:0o20666,device:0x200},
 {name:'/dev/console',mode:0o20600,device:0x201},
 {name:'/dev/gfx',mode:0o20666,device:0x400},
 {name:'/dev/null',mode:0o20666,device:0x100},
 {name:'/dev/zero',mode:0o20666,device:0x102},
 {name:'/dev/root',mode:0o60600,device:0},
 {name:'/dev/proc',mode:0o20666,device:0x103},
 {name:'/dev/urandom',mode:0o20444,device:0x104},
 {name:'/etc/passwd',mode:0o100644,data:Buffer.from('root:x:0:0:root:/root:/bin/sh\n')},
 {name:'/etc/group',data:Buffer.from('root:x:0:root\n')},
 {name:'/etc/mtab',data:Buffer.from('/dev/root / riplix rw 0 0\n')},
 {name:'/etc/termcap',data:Buffer.from('ansi|vt100|xterm|xterm-256color|ripple:co#80:li#30:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')},
 // Only the prompt setting was transcribed from the user's shell config. Never read
 // a personal disk while building this public seed.
 {name:'/root/.shrc',mode:0o100600,data:Buffer.from(String.raw`PS1='\[\e[1;32m\]\u@\h\[\e[0m\] \[\e[1;36m\]\w\[\e[0m\] \[\e[1;33m\]\$\[\e[0m\] '
export PS1
`)},
 {name:'/etc/nxlaunch.cnf',data:Buffer.from('Terminal - /bin/nxterm\nRift - /bin/nxterm /bin/rift\nCommander - /bin/nxterm /bin/mc\nProcesses - /bin/nxterm /bin/top\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nCalculator - /bin/nxcalc\nTetris - /bin/nxtetris\n')},
 {name:'/root/welcome.txt',data:Buffer.from('Welcome to Ripple, running in RVM.\nRiplix and its applications execute on the virtual Ripple CPU.\n\nrift / vi     Programmer\'s editor\nmc            RFL Commander file manager\nmux           Terminal tabs and split panes (Ctrl-B, then ? for help)\nman COMMAND   Read a command\'s manual\nawk, sed, make, basic, tclsh, bf, and the Unix command-line tools are installed.\nTry sokoban, wump, or hamurabi; BASIC examples are in /usr/share/basic.\nType startx to open NanoWM and NXDSKTOP.\nYour home is /root. Use cd or cd ~ to return to it.\nUse shutdown before exporting your disk to keep your files.\n')});
// C16 stores each guest byte in a 16-bit cell. Leave writable room while
// retaining the browser's 256 MiB disk cap; aliases share executable storage.
const filesystem=rootImage(files,114688,1024);
const disk=fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(path.join(kernelDirectory,'FUZIX.SYS'))},{name:'ROOTFS.IMG',data:filesystem}],
 {totalSectors:524288,sectorsPerFat:256,sectorsPerCluster:8,imageFactory:size=>new Image(size)});
writeImage(output,disk,true);
fs.writeFileSync(output+'.manifest.json',JSON.stringify({profile:'browser-offline-console',kernel:path.relative(root,path.join(kernelDirectory,'FUZIX.SYS')),files:manifest,aliases,resources:resources.map(f=>f.name)},null,2)+'\n',{flag:'wx'});
