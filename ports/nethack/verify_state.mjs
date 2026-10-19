// Verify only a disposable test disk, after the guest has shut down cleanly.
import {rootReader} from '../fuzix/tools/read-root.mjs';
import {openImage} from '../fuzix/tools/image.mjs';
import {fatFile} from '../fuzix/tools/fat.mjs';
const [disk,target,home,player='Smoke'] = process.argv.slice(2);
const image=openImage(disk),root=target==='native'?image:fatFile(image,'ROOTFS.IMG').data;
const reader=rootReader(root),w=p=>reader.checked.image.readUInt16LE(p*2);
if(reader.checked.issues.length) throw Error(reader.checked.issues.join('\n'));
for(const name of [home+'/.nethack',home+'/.nethack/save']) {
 const node=reader.lookup(name);
 if((w(node.base)&0o7777)!==0o700) throw Error('incorrect private directory mode: '+name);
}
for(const name of [home+'/.nethack/save/0'+player,home+'/.nethack/record']) {
 const node=reader.lookup(name);
 if((w(node.base)&0o7777)!==0o600) throw Error('incorrect private file mode: '+name);
 if(name.includes('/save/') && node.size<100) throw Error('save file is empty');
}
for(const prefix of ['/','/root/']) {
 const path=prefix+'.nethack';
 if(path===home+'/.nethack')continue;
 try {reader.lookup(path);} catch(error) {if(error.message.startsWith('missing '))continue;throw error;}
 throw Error('state escaped HOME: '+path);
}
console.log('Private HOME state and save verified.');
