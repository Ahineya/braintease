// Explicit image activation only; imported by Fuse-X, never x64/RVM installers.
import fs from 'node:fs';
export function bootFiles(profile) {
  if(profile!=='fuse-x')throw Error('unknown boot profile: '+profile);
  return [
    {name:'/etc/services',directory:true,mode:0o40755},
    {name:'/etc/boot',directory:true,mode:0o40755},
    {name:'/var/run',directory:true,mode:0o40755},
    {name:'/var/log',directory:true,mode:0o40755},
    {name:'/etc/boot/services.ini',mode:0o100644,preserve:true,
     data:Buffer.from('# Background services; installing definitions does not enable them.\n[services]\nenabled=\n')},
    {name:'/etc/boot/console.ini',mode:0o100644,preserve:true,
     data:Buffer.from('# First primary console only. Empty program starts a plain shell.\n[console]\nprogram=/bin/mux\n')},
    ...['hello.ini.example','disabled-profile.ini.example'].map(name=>({
      name:'/usr/share/doc/services/'+name,mode:0o100644,
      data:fs.readFileSync(new URL('./examples/'+name,import.meta.url))}))
  ];
}
