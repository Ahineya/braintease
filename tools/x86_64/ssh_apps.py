#!/usr/bin/env python3
"""Build the existing guest SSH implementation as static x64 RXE1 packages."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'ports/ssh'))
spec=importlib.util.spec_from_file_location('x64_ssh_build',ROOT/'ports/ssh/build.py')
ssh=importlib.util.module_from_spec(spec);spec.loader.exec_module(ssh)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,default=ROOT/'build/x86_64-system/ssh')
p.add_argument('--app',choices=['sshd','ssh-keygen','ssh','sftp-mount','all'],default='sshd')
a=p.parse_args();base=a.output.resolve();base.mkdir(parents=True,exist_ok=True)
report=[]
for name in (['sshd','ssh-keygen','ssh','sftp-mount'] if a.app=='all' else [a.app]):
 directory=base/('server' if name=='sshd' else 'client')
 objects=ssh.build(directory,ROOT/'target/release/rcc','native',3,client=name!='sshd')
 source=ROOT/'ports/ssh'/('sshd.c' if name=='sshd' else 'keygen.c' if name=='ssh-keygen' else 'mount.c' if name=='sftp-mount' else 'client.c')
 command=[sys.executable,str(ROOT/'fuse-x/cc.py'),str(source),'--static','--heap-profile','bounded',
          '--linker',str(ROOT/'tools/x86_64/app_link.py'),'--build',str(base/'sdk'),'-DWOLFSSL_USER_SETTINGS','-o',str(base/(name+'.rxe'))]
 for include in ([ROOT/'ports/ssh/config/client'] if name!='sshd' else [])+[ROOT/'ports/ssh',ROOT/'ports/ssh/config',directory/'source',directory/'crypto',ROOT/'ports/tls']:
  command+=['-I',str(include)]
 for obj in objects:command+=['--object-ir',str(obj)]
 subprocess.run(command,check=True)
 report.append({'name':name,'output':str(base/(name+'.rxe')),'status':'built; execution acceptance pending'})
(base/'build-result.json').write_text(json.dumps(report,indent=2)+'\n')
