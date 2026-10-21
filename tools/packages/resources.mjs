// Reuse the same data producers as OS image installation; no disk is opened.
import {commanderFiles,riftFiles} from '../../rfl/os/files.mjs';
import {manualFiles} from '../../ports/fuzix/tools/manuals.mjs';
import {sokobanFiles} from '../../ports/fuzix/tools/sokoban.mjs';
import {basicFiles} from '../../ports/basic/files.mjs';
import {lynxFiles} from '../../ports/lynx/files.mjs';
import {tlsFiles} from '../../ports/tls/files.mjs';
import {netsurfFiles} from '../../ports/netsurf/files.mjs';
import {unicodeFiles} from '../../libraries/unicode/files.mjs';
import {kernelCredits} from '../../ports/fuzix/tools/credits.mjs';
import {fuseDEFiles} from '../../ports/fusede/files.mjs';
import {kolfFiles} from '../../ports/kolf/files.mjs';
import {klotskiFiles} from '../../ports/klotski/files.mjs';
import {shikakuFiles} from '../../ports/shikaku/files.mjs';
import {compilerFiles} from '../../rcc-portable/files.mjs';
import fs from 'node:fs';import path from 'node:path';
const build=process.argv[2];
const groups={'commander-data':commanderFiles(),'rift-data':riftFiles(),
  'man-pages':manualFiles(),'sokoban-data':sokobanFiles(),'basic-data':basicFiles(),
  'lynx-data':lynxFiles(),'ca-certificates':tlsFiles(),'netsurf-data':netsurfFiles(),
  'unicode-data':unicodeFiles(),'system-licenses':kernelCredits(),
  'kolf-data':kolfFiles(),'klotski-data':klotskiFiles(),'shikaku-data':shikakuFiles()};
if(fs.existsSync(path.join(build,'fusede/theme.fdi')))groups['fusede-data']=fuseDEFiles(path.join(build,'fusede'));
groups.compiler=compilerFiles(path.join(build,'compiler'));
for(const [key,files] of Object.entries(groups))groups[key]=files.filter(f=>f.data&&!f.directory&&f.device===undefined).map(f=>({name:f.name,mode:(f.mode??0o100644)&0o777,data:Buffer.from(f.data).toString('base64')}));
for(const name of ['makeactions','makemacros']){
  (groups['make-data']??=[]).push({name:'/usr/lib/'+name,mode:0o644,data:fs.readFileSync(new URL('../../ports/fuzix/upstream/Applications/MWC/cmd/'+name,import.meta.url)).toString('base64')});
}
groups['nxlaunch-data']=[{name:'/etc/nxlaunch.cnf',mode:0o644,data:Buffer.from(
  'Terminal - /bin/nxterm\nMac-term - /bin/gterm\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nDraw - /bin/nxdraw\nCalculator - /bin/nxcalc\nTetris - /bin/nxtetris\nImages - /bin/nxview\nKlotski - /bin/klotski\nShikaku - /bin/shikaku\n').toString('base64')}];
console.log(JSON.stringify(groups));
