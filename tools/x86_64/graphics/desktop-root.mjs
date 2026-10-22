// Disposable public-source fixture: never reads a seed or personal root image.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage, atomicWrite} from '../../../ports/fuzix/tools/disk.mjs';
import {installRoot} from '../../../ports/fuzix/tools/install.mjs';
import {ptyDevices} from '../../../ports/fuzix/tools/pty.mjs';
const [output, apps, unix] = process.argv.slice(2);
if (!output || !apps || !unix) throw Error('usage: desktop-root.mjs OUTPUT GRAPHITE_APPS UNIX_APPS');
if (fs.existsSync(output)) throw Error('refusing to replace an existing fixture root');
const files = [];
const text = (name, data, mode=0o100644) => files.push({name, mode, data:Buffer.from(data)});
for (const name of ['init','sh','hello','check','cat','ls','pipecheck'])
  text(name==='init'?'/init':'/bin/'+name, `FXN1:${name}:1\n`, 0o100755);
for (const name of ['sh','ls','cat','ps','kill','sleep','mkdir','rm','stty','env','test','touch','df','sync'])
  files.push({name:'/bin/'+name,mode:0o100755,data:fs.readFileSync(path.join(unix,name+'.fxe'))});
const entries = [
  ['lvcalc','Calculator','calculator','Calculator'], ['lvselect','Files','folder-open','FilePicker'],
  ['lvgallery','Interface Gallery','grid-2x2','GraphiteGallery'], ['lvdata','Data Viewer','table-2','DataViewer'],
  ['lvedit','Text Editor','file-pen-line','TextEditor'], ['gterm','Terminal','terminal','Terminal'],
];
for (const [name,label,icon,id] of entries) {
  files.push({name:'/bin/'+name,mode:0o100755,data:fs.readFileSync(path.join(apps,name+'.fxe'))});
  text('/usr/share/applications/'+name+'.ini', `[application]\nid=org.riplix.${id}\nname=${label}\nicon=${icon}\ncategory=Applications\nexec=/bin/${name}\nkind=gui\npinned=true\n`);
}
for (const name of ['fusedeshell','screenshot','desktop-probe'])
  files.push({name:'/bin/'+name,mode:0o100755,data:fs.readFileSync(path.join(apps,name+'.fxe'))});
files.push(...ptyDevices(), {name:'/dev/gfx',mode:0o20666,device:0x400},
           {name:'/dev/null',mode:0o20666,device:0x100},
           {name:'/dev/tty',mode:0o20666,device:0x200});
text('/etc/passwd','root:x:0:0:root:/root:/bin/sh\n');
text('/root/.profile','HOME=/root; export HOME\nPS1="root@riplix ~ # "; export PS1\n');
text('/root/hello.txt','Hello from the native Graphite desktop.\n');
text('/root/measurements.csv',fs.readFileSync(new URL('../../../ports/lvgl/examples/measurements.csv',import.meta.url)));
text('/bin/startgraphite','#!/bin/sh\nexec /bin/fusedeshell\n',0o100755);
atomicWrite(output, installRoot(rootImage([],262144,2048),files));
console.log('Fresh native desktop fixture:',output);
