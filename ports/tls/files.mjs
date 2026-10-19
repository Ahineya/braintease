import fs from 'node:fs';
import crypto from 'node:crypto';
export function tlsFiles() {
  const data=fs.readFileSync(new URL('./roots/cert.pem',import.meta.url));
  const source=JSON.parse(fs.readFileSync(new URL('./roots/source.json',import.meta.url),'utf8'));
  if(crypto.createHash('sha256').update(data).digest('hex')!==source.sha256)
    throw Error('Guest TLS trust bundle differs from its pinned checksum');
  return [{name:'/etc/ssl/cert.pem',mode:0o100644,preserve:true,data}];
}
