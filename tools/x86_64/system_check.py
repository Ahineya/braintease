"""Existing network regression plus real OpenSSH/SFTP on a disposable x64 VM."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
from network_check import NetworkCheck

class SystemCheck(NetworkCheck):
    def __init__(self, command, port, output, ssh_key=None, ssh_port=2325):
        super().__init__(command,port,output,supervised=True)
        self.ssh_key=Path(ssh_key).resolve() if ssh_key else None
        self.ssh_port=ssh_port
    def run(self,shell,transcript,deadline,restart,qmp):
        super().run(shell,transcript,deadline,restart,qmp)
        if not self.ssh_key:return
        shell('entropycheck',r'entropycheck passed')
        shell('svc --runtime /var/run/svcd restart sshd',r'OK restart requested')
        until=min(deadline,time.monotonic()+75)
        while True:
            text=shell('svc --runtime /var/run/svcd status',None)
            if re.search(r'sshd running ',text):break
            if re.search(r'ssh-keys failed ',text):
                shell('cat /var/run/svcd/ssh-keys.log',None)
                raise RuntimeError('guest host-key provisioning failed')
            if time.monotonic()>until:
                shell('cat /var/run/svcd/sshd.log',None)
                raise RuntimeError('SSH did not signal service readiness')
        public=shell('ssh-keygen -y -f /etc/ssh/hostkey.der',r'ecdsa-sha2-nistp256 ')
        match=re.search(r'ecdsa-sha2-nistp256 [A-Za-z0-9+/=]+',public)
        if not match:raise RuntimeError('guest host public key missing')
        known=self.output/'known-hosts'
        known.write_text(f'[127.0.0.1]:{self.ssh_port} '+match[0]+'\n')
        options=['-F','/dev/null','-o','BatchMode=yes','-o','IdentitiesOnly=yes',
                 '-o','StrictHostKeyChecking=yes','-o',f'UserKnownHostsFile={known}',
                 '-o','ConnectTimeout=10','-i',str(self.ssh_key)]
        host='root@127.0.0.1'
        operation=0
        def run(command,ok=0):
            nonlocal operation
            operation+=1;prefix=self.output/f'ssh-operation-{operation}'
            with prefix.with_suffix('.out').open('wb') as out,prefix.with_suffix('.err').open('wb') as err:
                child=subprocess.Popen(command,stdout=out,stderr=err)
                stop=min(deadline,time.monotonic()+150);progress=time.monotonic()+30
                try:
                    while child.poll() is None:
                        if time.monotonic()>stop:raise RuntimeError(f'SSH operation{operation} timed out; inspect {prefix}.err')
                        if time.monotonic()>progress:
                            print(f'SSH operation{operation} still running',flush=True);progress+=30
                        time.sleep(.1)
                finally:
                    if child.poll() is None:child.kill();child.wait()
            stdout=prefix.with_suffix('.out').read_bytes();stderr=prefix.with_suffix('.err').read_bytes()
            if child.returncode!=ok:raise RuntimeError(f'SSH interoperability failed status{child.returncode}: {stderr[-2000:]!r}')
            return subprocess.CompletedProcess(command,child.returncode,stdout,stderr)
        cmd=['ssh',*options,'-p',str(self.ssh_port),host]
        r=run([*cmd,'echo X64_SSH_EXEC_OK; echo X64_STDERR >&2; exit 37'],37)
        if b'X64_SSH_EXEC_OK' not in r.stdout or b'X64_STDERR' not in r.stderr:raise RuntimeError('SSH exec streams differ')
        (self.output/'ssh-exec-result.json').write_text(json.dumps({'status':'passed','remote_exit_status':37,'stdout':r.stdout.decode(),'stderr':r.stderr.decode()},indent=2)+'\n')
        payload=bytes(range(256))*257+b'X64_SFTP\n'
        with tempfile.TemporaryDirectory(prefix='transfer-',dir=self.output) as d:
            d=Path(d);source=d/'upload';dest=d/'download';source.write_bytes(payload)
            batch=d/'batch';batch.write_text(f'put {source} /tmp/ssh-transfer\nget /tmp/ssh-transfer {dest}\n')
            run(['sftp','-v',*options,'-P',str(self.ssh_port),'-b',str(batch),host])
            if dest.read_bytes()!=payload:raise RuntimeError('SFTP byte mismatch')
        self.results[-1]['ssh']={'exec_stderr_exit_status':True,'sftp_bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest(),'host_key_pinned_from_guest_console':True}
        (self.output/'ssh-result.json').write_text(json.dumps(self.results,indent=2)+'\n')
        print('PASS: x64 entropy, supervised SSH readiness, OpenSSH exec/stderr/status and binary SFTP',flush=True)
