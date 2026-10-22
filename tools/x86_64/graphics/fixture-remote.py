#!/usr/bin/env python3
"""Copy only the public disposable GUI fixture to a private Ubuntu KVM folder."""
from pathlib import Path
import io,json,os,stat,subprocess,tarfile,uuid
ROOT=Path(__file__).resolve().parents[3];w=ROOT/'build/x86_64-graphics/fixture'
ssh=['ssh','-F','/dev/null','-o','BatchMode=yes','-o','PubkeyAuthentication=no','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(ROOT/'build/x86_64-internal-boot/ubuntu-known-hosts'),'-o','ConnectTimeout=6','-p','2222','root@192.168.100.107']
remote='/tmp/riplix-gui-'+uuid.uuid4().hex[:12]
files=['usb-boot.img','root.img','edk2-x86_64-code.fd','edk2-i386-vars.fd','run.py','fixture.json']
archive=w.parent/'fixture-input.tar.gz'
with tarfile.open(archive,'w:gz',compresslevel=1) as t:
    for name in files:
        source=w/name
        if not stat.S_ISREG(source.lstat().st_mode):raise RuntimeError('fixture input is not regular: '+name)
        t.add(source,arcname=name,recursive=False)
program='import os,sys,tarfile; p='+repr(remote)+'; os.mkdir(p,0o700); t=tarfile.open(fileobj=sys.stdin.buffer,mode="r|gz"); t.extractall(p,filter="data")'
with archive.open('rb') as f:subprocess.run([*ssh,'python3 -c '+__import__('shlex').quote(program)],stdin=f,check=True)
(w/'remote.json').write_text(json.dumps({'directory':remote,'only_public_disposable_fixture':True,'files':files},indent=2)+'\n')
print('VM directory:',remote,flush=True)
result=subprocess.run([*ssh,'python3 '+remote+'/run.py'],capture_output=True)
(w/'remote-run.log').write_bytes(result.stdout+result.stderr)
# Results originate only from this public VM: its private disk has no HP keys.
program='''import os,sys,tarfile
p='''+repr(remote)+'''
names=[n for n in ['result.json','failure.json','serial.log','qemu.log','qmp.ppm','console.ppm','root.img'] if os.path.isfile(p+'/'+n)]
with tarfile.open(fileobj=sys.stdout.buffer,mode='w|gz',compresslevel=1) as t:
 for n in names:t.add(p+'/'+n,arcname=n,recursive=False)
'''
with (w.parent/'fixture-results.tar.gz').open('wb') as f:subprocess.run([*ssh,'python3 -c '+__import__('shlex').quote(program)],stdout=f,check=True)
out=w/('result-files-'+remote.rsplit('-',1)[1]);out.mkdir(exist_ok=True)
with tarfile.open(w.parent/'fixture-results.tar.gz') as t:t.extractall(out,filter='data')
(w/'latest-remote.json').write_text(json.dumps({'directory':remote,'local_results':str(out)},indent=2)+'\n')
print(result.stdout.decode(errors='replace'),end='')
if result.returncode:
    print(result.stderr.decode(errors='replace'),end='');raise SystemExit(result.returncode)
