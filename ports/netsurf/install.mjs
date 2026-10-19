// SPDX-License-Identifier: GPL-2.0-only
// Produce a checked image; in-place upgrades require a stopped, clean disk.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {openImage,writeImage} from '../fuzix/tools/image.mjs';
import {installRoot} from '../fuzix/tools/install.mjs';
import {checkRoot} from '../fuzix/tools/fsck.mjs';
import {fatFile} from '../fuzix/tools/fat.mjs';
import {tlsFiles} from '../tls/files.mjs';
import {unicodeFiles} from '../../libraries/unicode/files.mjs';
import {netsurfFiles} from './files.mjs';
import {spawnSync} from 'node:child_process';
import {atomicWrite} from '../fuzix/tools/disk.mjs';
import {backupImage,pruneImageBackups} from '../fuzix/tools/image-backup.mjs';
const here=path.dirname(fileURLToPath(import.meta.url));
const args=process.argv.slice(2), inplace=args.includes('--in-place');
if(inplace)args.splice(args.indexOf('--in-place'),1);
const [target,input,...paths]=args;
const [output,program,worker,server,...rest]=inplace?[input,...paths]:paths;
if(!['ripple','native'].includes(target)||!input||!output||rest.length)
    throw Error('usage: install.mjs [--in-place] ripple|native INPUT-IMAGE [NEW-OUTPUT-IMAGE] [EXECUTABLE [HTTP-WORKER [NANO-X-SERVER]]]');
if(!inplace&&fs.existsSync(output))throw Error('output exists; choose a new filename');
function stopped() {
    if(!inplace)return;
    const state=spawnSync('lsof',['-t','--',fs.realpathSync(input)],{encoding:'utf8'});
    if(state.error||![0,1].includes(state.status))throw Error('cannot verify that the image is stopped (lsof required)');
    if(state.status===0&&state.stdout.trim().split(/\s+/).some(pid=>Number(pid)!==process.pid))
        throw Error('image is open; shut down its guest before installing NetSurf');
}
stopped();
const executable=program||path.join(here,'../../build/netsurf/netsurf.'+(target==='ripple'?'rxe':'fxe'));
const data=fs.readFileSync(executable);
if(data.subarray(0,target==='ripple'?4:8).toString()!==(target==='ripple'?'RXE1':'FUSEX001'))
    throw Error('executable does not match the selected target');
const helper=fs.readFileSync(worker||path.join(path.dirname(executable),'htget.'+(target==='ripple'?'rxe':'fxe')));
if(helper.subarray(0,target==='ripple'?4:8).toString()!==(target==='ripple'?'RXE1':'FUSEX001'))
    throw Error('HTTP worker does not match the selected target');
const files=[{name:'/bin/netsurf',mode:0o100755,data},
    {name:'/bin/htget',mode:0o100755,data:helper},...tlsFiles(),...unicodeFiles(),
    ...netsurfFiles()];
if(server) {
    const bytes=fs.readFileSync(server);
    if(bytes.subarray(0,target==='ripple'?4:8).toString()!==(target==='ripple'?'RXE1':'FUSEX001'))
        throw Error('Nano-X server does not match the selected target');
    files.push({name:'/bin/nano-X',mode:0o100755,data:bytes});
}
const original=openImage(input), wrapped=target==='ripple'?fatFile(original,'ROOTFS.IMG'):null;
const root=wrapped?wrapped.data:original;
const before=checkRoot(root);
if(before.issues.length)throw Error('input filesystem must be clean: '+before.issues.join('; '));
const installed=installRoot(root,files);
if(checkRoot(installed).issues.length)throw Error('installed filesystem did not pass validation');
const updated=wrapped?wrapped.replace(installed):installed;
stopped();
if(inplace) {
    backupImage(input,original);
    atomicWrite(input,updated);
    pruneImageBackups(input);
} else writeImage(output,updated,true);
console.log(`Installed NetSurf, guest HTTP worker, trust bundle and examples into ${output}; ${inplace?'updated atomically':'source image unchanged'}`);
