// Explicit x64 activation profile, separate from hosted installer defaults.
import fs from 'node:fs';
import path from 'node:path';
import {wifiRecoveryFiles} from './wifi-recovery.mjs';
const file=(name,text,mode=0o100644,preserve=true)=>({name,mode,preserve,data:Buffer.from(text)});
export function systemFiles(config,sshTools=null,sshKey=null,wifi=false) {
  config ??= {address:'192.168.64.250',netmask:'255.255.255.0',gateway:'192.168.64.1',dns:'192.168.64.3'};
  const files=[];
  for(const name of ['/var/log','/var/run','/etc/services','/etc/boot','/root','/tmp'])
    files.push({name,directory:true,mode:name==='/tmp'?0o41777:0o40755});
  files.push({name:'/dev/urandom',mode:0o20666,device:0x104});
  files.push(file('/etc/net-start.sh',
    '# Address configuration; radio association remains native during transition.\n'+
    `/bin/ifconfig eth0 ${config.address} netmask ${config.netmask} gateway ${config.gateway} up || exit 1\n`+
    'exec /bin/net-ready\n',0o100755));
  files.push(file('/etc/services/network.ini','[service]\ntype=oneshot\nprogram=/bin/sh\nrestart=on-failure\nrestart_limit=5\nrestart_delay_ms=1000\n[arguments]\n1=/etc/net-start.sh\n'));
  files.push(file('/etc/services/telnetd.ini','[service]\nprogram=/bin/telnetd\nrequires=network\nready=exec\nrestart=on-failure\nrestart_limit=5\nrestart_delay_ms=1000\n'));
  files.push(file('/etc/boot/services.ini','[services]\nenabled=network telnetd\n'));
  files.push(file('/etc/boot/console.ini','[console]\nprogram=\n'));
  const base=new URL('../../../fuse-x-shared/packages/bootstrap/x64/',import.meta.url);
  for(const name of ['pkg-engine','repo.pub','install.sh']) {
    const data=fs.readFileSync(new URL(name,base));
    if(name==='pkg-engine' && (data.toString('ascii',0,8)!=='FUSEX001'||data.readUInt32LE(8)!==0x31455852))throw Error('x64 pkg bootstrap must be RXE1');
    files.push({name:'/root/pkg-setup/'+name,mode:name==='pkg-engine'?0o100755:0o100644,data});
  }
  files.push(file('/root/INSTALL-PKG','Run: sh /root/pkg-setup/install.sh\nThen: PATH=/usr/bin:/bin; export PATH\nRepository key is pinned from authenticated publisher setup.\n'));
  if(sshTools) {
    const key=fs.readFileSync(sshKey,'utf8');
    if(!/^ecdsa-sha2-nistp256 [A-Za-z0-9+/=]+(?: [^\r\n]*)?\r?\n?$/.test(key))throw Error('expected one P-256 OpenSSH public key');
    for(const name of ['sshd','ssh-keygen','ssh']) {
      const data=fs.readFileSync(path.join(sshTools,name+'.rxe'));
      if(data.toString('ascii',0,8)!=='FUSEX001'||data.readUInt32LE(8)!==0x31455852)throw Error('SSH tools must be x64 RXE1');
      files.push({name:'/bin/'+name,mode:0o100755,data});
    }
    for(const name of ['sftp','scp'])files.push({name:'/bin/'+name,mode:0o100755,link:'/bin/ssh'});
    files.push({name:'/etc/ssh',directory:true,mode:0o40700},{name:'/root/.ssh',directory:true,mode:0o40700});
    files.push(file('/root/.ssh/authorized_keys',key,0o100600));
    files.push(file('/etc/ssh-setup.sh','if test -s /etc/ssh/hostkey.der; then exit 0; fi\nexec /bin/sshd --keygen /etc/ssh/hostkey.der\n',0o100700));
    files.push(file('/etc/services/ssh-keys.ini','[service]\ntype=oneshot\nprogram=/bin/sh\nrestart=no\nstart_timeout_ms=60000\n[arguments]\n1=/etc/ssh-setup.sh\n'));
    files.push(file('/etc/services/sshd.ini','[service]\nprogram=/bin/sshd\nrequires=network ssh-keys\nready=notify\nrestart=on-failure\nrestart_limit=5\nrestart_delay_ms=1000\nstart_timeout_ms=60000\nstop_timeout_ms=5000\n'));
    const profile=files.find(f=>f.name==='/etc/boot/services.ini');profile.data=Buffer.from('[services]\nenabled=network telnetd sshd\n');
  }
  if(wifi){
    const at=files.findIndex(f=>f.name==='/etc/services/network.ini');files.splice(at,1);
    files.push(...wifiRecoveryFiles(Boolean(sshTools)));
  }
  return files;
}
