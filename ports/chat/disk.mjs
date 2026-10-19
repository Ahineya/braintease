// Called by disk.py while holding the image and offline-tool locks.
import fs from 'node:fs';
import {openImage} from '../fuzix/tools/image.mjs';
import {fatFile} from '../fuzix/tools/fat.mjs';
import {rootReader} from '../fuzix/tools/read-root.mjs';
import {installRoot} from '../fuzix/tools/install.mjs';
import {layout} from '../fuzix/tools/layout.mjs';
import {atomicWrite} from '../fuzix/tools/disk.mjs';
try {
  const [locked,action,image,file,state,...extra]=process.argv.slice(2);
  if(locked!=='--locked'||extra.length||!['request','credentials'].includes(action)||!state.startsWith('/')||state.includes('..')) throw Error('Use disk.py');
  const original=openImage(image);
  let fat,root=original;
  try {layout(original);} catch {
    fat=fatFile(original,'ROOTFS.IMG');root=fat.data;
  }
  const reader=rootReader(root);
  if(reader.checked.issues.length) throw Error('Guest filesystem is not clean; shut down or check it first');
  const read=path=>String.fromCharCode(...reader.read(path));
  const host=read(state+'/host-id');
  if(!/^urn:uuid:[0-9a-f-]{36}$/.test(host)) throw Error('Start chat once in this guest to create its host ID');
  if(action==='request') {
    const fd=fs.openSync(file,'wx',0o600);
    try {fs.writeFileSync(fd,JSON.stringify({format:1,agent_name:'Fuse Chat',ext_agent_host_id:host})+'\n');} finally {fs.closeSync(fd);}
    console.log('Public guest login request exported. Run login.py with this request.');
  } else {
    const st=fs.lstatSync(file);
    if(!st.isFile()||(st.mode&0o077)||st.uid!==process.getuid()) throw Error('Credential file must be yours and mode 0600');
    const text=fs.readFileSync(file,'utf8'),account=JSON.parse(text);
    if(account.ext_agent_host_id!==host||account.issuer!=='https://auth.openai.com'||account.token_type!=='Bearer'||!account.client_id?.startsWith('oaiapp_')||!account.access_token||!account.refresh_token||!account.subject||!Number.isInteger(account.expires_at)||!account.scopes?.includes('chatgpt.tokens.use.direct')||!account.scopes?.includes('resource.invoke')) throw Error('Invalid credentials or different guest host ID');
    const updated=installRoot(root,[{name:state+'/account.json',mode:0o100600,data:Buffer.from(text)}]);
    const output=fat?fat.replace(updated):updated;
    atomicWrite(image,output);fs.chmodSync(image,0o600);
    console.log('Credentials installed in the stopped guest. Remove the transfer file after booting successfully.');
  }
} catch(error) {console.error('Chat disk transfer: '+error.message);process.exitCode=1;}
