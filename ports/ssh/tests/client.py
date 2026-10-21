#!/usr/bin/env python3
"""Guest clients against independent OpenSSH through a blind Ethernet relay."""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import fcntl
import struct
import termios
import signal
import time
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
sys.path.insert(0,str(ROOT/'ports/fuzix/tools'))
from guest import Guest
from network_peer import Peer

class Outbound(Peer):
    def __init__(self,path,port):
        super().__init__(path);self.port=port;self.clients={};self.tcp_services[2222]=lambda s:None
    def pump(self):
        super().pump()
        for key,s in self.streams.items():
            if key not in self.clients:
                sock=socket.create_connection(('127.0.0.1',self.port));sock.setblocking(False)
                self.clients[key]=dict(socket=sock,at=0,closed=False,eof=False)
            c=self.clients[key];sock=c['socket']
            if c['closed']:continue
            if c['at']<len(s.received):
                try:c['at']+=sock.send(s.received[c['at']:])
                except BlockingIOError:pass
                except (BrokenPipeError,ConnectionResetError):s.closing=True;c['closed']=True;sock.close();continue
            if s.fin and not c['eof']:
                sock.shutdown(socket.SHUT_WR);c['eof']=True
            if len(s.pending)<65536:
                try:
                    data=sock.recv(16384)
                    if data:s.queue(data)
                    else:s.closing=True;c['closed']=True;sock.close()
                except BlockingIOError:pass
                except ConnectionResetError:s.closing=True;c['closed']=True;sock.close()
    def close(self):
        for c in self.clients.values():c['socket'].close()
        super().close()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    parser.add_argument('--output',type=Path,default=ROOT/'build/ssh/client-tests')
    parser.add_argument('--key-type',choices=('ecdsa','rsa','ed25519'),default='ecdsa')
    parser.add_argument('--rsa-bits',choices=(2048,3072,4096),type=int,default=3072)
    parser.add_argument('--features-only',action='store_true')
    parser.add_argument('--login-only',action='store_true')
    parser.add_argument('--trust-only',action='store_true',help='only interactive first-use trust checks')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    with (ROOT/'build/ssh/client'/(args.target+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_SH)
        run_tests(args)

def run_tests(args):
    executable=ROOT/'build/ssh'/('ssh.fxe' if args.target=='native' else 'ssh.rxe')
    (args.output/'input.json').write_text(json.dumps(dict(target=args.target,executable=str(executable),sha256=hashlib.sha256(executable.read_bytes()).hexdigest()),indent=2)+'\n')
    with tempfile.TemporaryDirectory(prefix='ssh-client-',dir='/tmp') as directory:
        work=Path(directory).resolve();work.chmod(0o700)
        def run(cmd):subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        for name in ('host','changed','user','wrong'):
            command=['ssh-keygen','-q','-t',args.key_type,'-N','','-f',str(work/name)]
            if args.key_type!='ed25519':command+=['-b',str(args.rsa_bits) if args.key_type=='rsa' else '256','-m','PEM']
            run(command)
        # macOS ssh-keygen emits explicit curve parameters in PEM. The guest
        # profile deliberately accepts only the named NIST curves.
        for name in (('user','wrong') if args.key_type=='ecdsa' else ()):
            run(['openssl','ec','-in',str(work/name),'-param_enc','named_curve','-out',str(work/(name+'.named'))])
            os.replace(work/(name+'.named'),work/name)
        (work/'authorized_keys').write_bytes((work/'user.pub').read_bytes());(work/'authorized_keys').chmod(0o600)
        probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
        user=os.environ.get('USER') or subprocess.check_output(['id','-un'],text=True).strip()
        config=work/'sshd_config';config.write_text(f'''Port {port}
ListenAddress 127.0.0.1
HostKey {work/'host'}
PidFile {work/'pid'}
AuthorizedKeysFile {work/'authorized_keys'}
StrictModes no
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
AllowUsers {user}
Subsystem sftp internal-sftp
LogLevel VERBOSE
''')
        log=(args.output/'openssh.log').open('w');server=subprocess.Popen(['/usr/sbin/sshd','-D','-e','-f',str(config)],stdout=log,stderr=log)
        peer=None;guest=None
        try:
            until=time.monotonic()+5
            while True:
                if server.poll() is not None:raise AssertionError((args.output/'openssh.log').read_text())
                try:s=socket.create_connection(('127.0.0.1',port),.1);s.close();break
                except OSError:
                    if time.monotonic()>until:raise
                    time.sleep(.02)
            expected=bytes(range(256))*384+b'binary transfer\0\xff\n';(work/'binary').write_bytes(expected)
            host='[192.168.64.1]:2222';pub=(work/'host.pub').read_text().split();changed=(work/'changed.pub').read_text().split()
            known=f'{host} {pub[0]} {pub[1]} fixture host comment with many words\n'
            salt=b'independent test salt';hashed='|1|'+base64.b64encode(salt).decode()+'|'+base64.b64encode(hmac.new(salt,host.encode(),hashlib.sha1).digest()).decode()
            suffix='.fxe' if args.target=='native' else '.rxe';exe=ROOT/'build/ssh'/('ssh'+suffix)
            files=[dict(name='/root/.ssh/id_ecdsa',mode=0o100600,path=str(work/'user')),
                   dict(name='/root/.ssh/wrong',mode=0o100600,path=str(work/'wrong')),
                   dict(name='/root/.ssh/known_hosts',mode=0o100600,text=known),
                   dict(name='/root/.ssh/changed',mode=0o100600,text=f'{host} {changed[0]} {changed[1]}\n'),
                   dict(name='/root/.ssh/revoked',mode=0o100600,text=known+f'@revoked {host} {pub[0]} {pub[1]}\n'),
                   dict(name='/root/.ssh/hashed',mode=0o100600,text=f'{hashed} {pub[0]} {pub[1]}\n'),
                   dict(name='/root/.ssh/config',mode=0o100600,text=f'Host fixture\n HostName 192.168.64.1\n User {user}\n Port 2222\n StrictHostKeyChecking yes\n BatchMode yes\n'),
                   dict(name='/binary',path=str(work/'binary'))]
            peer=Outbound(work/'nic',port)
            guest=Guest(work,args.target,{'ssh':exe,'mkdir':ROOT/('fuse-x/build/apps' if args.target=='native' else 'build/fuzix')/('mkdir'+suffix),'ls':ROOT/('fuse-x/build/apps' if args.target=='native' else 'build/fuzix')/('ls'+suffix),'ln':ROOT/('fuse-x/build/apps' if args.target=='native' else 'build/fuzix')/('ln'+suffix),'ssh-keygen':ROOT/'build/ssh'/('ssh-keygen'+suffix)},files,peer,blocks=65536,code_memory=None if args.target=='native' else 8388608)
            def command(cmd,status=0,seconds=180):
                text=guest.shell(cmd+'; echo CLIENT_STATUS:$?',seconds)
                (args.output/'guest.log').write_bytes(guest.log)
                assert f'CLIENT_STATUS:{status}\n' in text,text
                return text
            command('ln /bin/ssh /bin/scp; ln /bin/ssh /bin/sftp')
            if args.trust_only:
                for answer,status in ((b'no',255),(b'yes',0)):
                    os.write(guest.master,b'ssh -o UserKnownHostsFile=/root/.ssh/ask -o StrictHostKeyChecking=ask -o BatchMode=no fixture true; echo TRUST_STATUS:$?\r')
                    guest.read(b'Type yes: ')
                    os.write(guest.master,answer+b'\r')
                    text=guest.read(guest.prompt);assert f'TRUST_STATUS:{status}\n' in text,text
                command('ssh -o UserKnownHostsFile=/root/.ssh/ask fixture true')
                print('PASS default first-use trust rejection, explicit confirmation and persistent host verification',flush=True)
                return
            if args.features_only or args.login_only:
                text=command("ssh fixture 'echo KEY_AUTH_OK'")
                assert '\nKEY_AUTH_OK\n' in text,text
                assert ('ED25519' if args.key_type=='ed25519' else 'RSA' if args.key_type=='rsa' else 'ECDSA') in (args.output/'openssh.log').read_text()
                if args.login_only:
                    print(f'PASS {args.key_type} host-key verification, user authentication and remote command',flush=True)
                    return
                command('mkdir /tree; mkdir /tree/nested; mkdir /tree/empty')
                command('cat /binary > /tree/nested/data; echo spaces > "/tree/a b"')
                command('echo full-name > /tree/'+'n'*30)
                command(f'scp -r /tree/ fixture:{work}/uploaded',seconds=300)
                assert (work/'uploaded/nested/data').read_bytes()==expected
                assert (work/'uploaded/empty').is_dir() and (work/'uploaded/a b').read_bytes()==b'spaces\n'
                assert (work/('uploaded/'+'n'*30)).read_bytes()==b'full-name\n'
                command(f'scp -r fixture:{work}/uploaded/ /roundtrip',seconds=300)
                command(f'scp -r fixture:{work}/uploaded/ /roundtrip',seconds=300)
                command(f'scp /binary fixture:{work}/single')
                command(f'scp fixture:{work}/single /flat')
                (work/'badnames').mkdir();(work/'badnames'/('x'*31)).write_text('must not truncate')
                command(f'scp -r fixture:{work}/badnames /badnames',1)
                command('ls /badnames')
                (work/'links').mkdir();(work/'links/loop').symlink_to(work/'links',target_is_directory=True)
                command(f'scp -r fixture:{work}/links /links',1)
                command('ln -s /tree /tree/loop')
                command(f'scp -r /tree fixture:{work}/refused',1)
                command('ln -s /roundtrip /destlink')
                command(f'scp -r fixture:{work}/uploaded /destlink',1)
                # A connected peer that never sends its SSH identification
                # must still hit the configured idle timeout.
                silent=socket.socket();silent.bind(('127.0.0.1',0));silent.listen(2)
                previous=peer.port;peer.port=silent.getsockname()[1]
                try:
                    text=command('ssh -o ConnectTimeout=1 fixture true',255,seconds=60)
                    assert 'timed out' in text.lower(),text
                finally:peer.port=previous;silent.close()
                guest.close();guest=None
                if args.target=='native':
                    script="import {openImage} from './ports/fuzix/tools/image.mjs';import {rootReader} from './ports/fuzix/tools/read-root.mjs';process.stdout.write(Buffer.from(rootReader(openImage(process.argv[1])).read(process.argv[2])));"
                    def read(path):return subprocess.check_output(['node','--input-type=module','-e',script,str(work/'disk.img'),path],cwd=ROOT)
                else:
                    from root_reader import RootReader
                    reader=RootReader(work/'disk.img')
                    def read(path):return bytes(reader.read(path))
                assert read('/roundtrip/nested/data')==expected and read('/flat')==expected
                assert read('/roundtrip/'+'n'*30)==b'full-name\n'
                print(f'PASS {args.key_type} host/user authentication, recursive trees/empty directories/spaces/full-length names, overwrite, symlink/long-name rejection, idle-timeout rejection and binary comparison',flush=True)
                return
            text=command("ssh fixture 'printf COMMAND_OK; printf ERROR_OK >&2; exit 7'",7)
            assert 'COMMAND_OK' in text and 'ERROR_OK' in text,text
            print('PASS remote command, stderr, status and ~/.ssh/config',flush=True)
            command(f"ssh fixture 'cat > {work}/stdin' < /binary")
            assert (work/'stdin').read_bytes()==expected
            command(f"ssh fixture 'cat {work}/binary' > /exec-download")
            command("ssh fixture 'echo EARLY_EXIT; exit 9' < /binary",9)
            print('PASS SSH binary stdin/stdout and early remote exit',flush=True)
            # A real remote PTY, not a line-oriented fake shell.
            text=command("ssh -t fixture 'stty size; echo PTY_OK'")
            assert '24 80' in text and '\nPTY_OK\n' in text,text
            os.write(guest.master,b"ssh -t fixture 'PS1=REMOTE_PROMPT; export PS1; /bin/sh -i'\r")
            guest.read(b'\nREMOTE_PROMPT')
            os.write(guest.master,b'cat\r')
            guest.read(b'cat\r\n')
            os.write(guest.master,b'INTERACTIVE_INPUT\r')
            guest.read(b'INTERACTIVE_INPUT\r\n');guest.read(b'INTERACTIVE_INPUT\r\n')
            os.write(guest.master,b'\x03')
            guest.read(b'REMOTE_PROMPT')
            fcntl.ioctl(guest.master,termios.TIOCSWINSZ,struct.pack('HHHH',37,91,0,0))
            os.killpg(guest.process.pid,signal.SIGWINCH)
            deadline=time.monotonic()+.3
            while time.monotonic()<deadline:guest.pump()
            os.write(guest.master,b'stty size; echo RESIZED\r')
            text=guest.read(b'\r\nRESIZED\r\n');assert '37 91' in text,text
            guest.read(b'REMOTE_PROMPT')
            os.write(guest.master,b'exit\r');guest.read(guest.prompt)
            print('PASS interactive PTY input, Ctrl-C isolation, resize and restoration',flush=True)
            command('ssh -i /root/.ssh/wrong fixture true',255)
            command('ssh -o UserKnownHostsFile=/root/.ssh/changed fixture true',255)
            command('ssh -o UserKnownHostsFile=/root/.ssh/revoked fixture true',255)
            command('ssh -o UserKnownHostsFile=/root/.ssh/missing fixture true',255)
            command('ssh -o UserKnownHostsFile=/root/.ssh/hashed fixture true')
            command('ssh -o UserKnownHostsFile=/root/.ssh/new -o StrictHostKeyChecking=accept-new fixture true')
            command('ssh -o UserKnownHostsFile=/root/.ssh/new fixture true')
            print('PASS wrong identity, changed/revoked/unknown keys, hashed known_hosts and persistent accept-new',flush=True)
            for bits in (256,384,521):
                key=f'/root/.ssh/generated{bits}'
                command(f'ssh-keygen -b {bits} -f {key} -N ""')
                text=guest.shell(f'cat {key}.pub')
                public=next(l for l in text.splitlines() if l.startswith('ecdsa-sha2-'))
                with (work/'authorized_keys').open('a') as auth:auth.write(public+'\n')
                command(f'ssh -i {key} fixture true')
            command('ssh-keygen -p -f /root/.ssh/generated256 -P "" -N "test secret"')
            command('ssh -i /root/.ssh/generated256 fixture true',255)
            os.write(guest.master,b'ssh -o BatchMode=no -i /root/.ssh/generated256 fixture true; echo ENCRYPTED_STATUS:$?\r')
            guest.read(b'Enter key passphrase: ')
            os.write(guest.master,b'test secret\r')
            text=guest.read(guest.prompt);assert 'ENCRYPTED_STATUS:0\n' in text,text
            print('PASS guest-generated ECDSA 256/384/521 identities and encrypted-key login',flush=True)
            command(f'scp /binary fixture:{work}/upload')
            assert (work/'upload').read_bytes()==expected
            command(f'scp fixture:{work}/binary /download')
            # Avoid binary console output; compare independently after shutdown.
            batch=f'''pwd
ls {work}
mkdir {work}/sub
put /binary {work}/sub/original
rename {work}/sub/original {work}/sub/renamed
chmod 600 {work}/sub/renamed
get {work}/sub/renamed /sftp-download
rm {work}/sub/renamed
rmdir {work}/sub
quit
'''
            # Write batch through the shell using its ordinary quoted multiline argument.
            command("echo '"+batch+"' > /batch")
            command('sftp -b /batch fixture',seconds=240)
            command("echo 'get /nonexistent /download' > /bad-batch")
            command('sftp -b /bad-batch fixture',1)
            print('PASS scp and SFTP binary transfers, directory commands and failed-download isolation',flush=True)
            guest.close();guest=None
            if args.target=='native':
                script="import {openImage} from './ports/fuzix/tools/image.mjs';import {rootReader} from './ports/fuzix/tools/read-root.mjs';process.stdout.write(Buffer.from(rootReader(openImage(process.argv[1])).read(process.argv[2])));"
                def read(path):return subprocess.check_output(['node','--input-type=module','-e',script,str(work/'disk.img'),path],cwd=ROOT)
            else:
                from root_reader import RootReader
                reader=RootReader(work/'disk.img')
                def read(path):return bytes(reader.read(path))
            assert read('/exec-download')==expected
            assert read('/download')==expected
            assert read('/sftp-download')==expected
            print('PASS independent binary comparison, clean shutdown and filesystem validation',flush=True)
        finally:
            try:
                if guest:
                    if guest.process.poll() is None and guest.prompt not in guest.log[-64:]:
                        os.write(guest.master,b'\x03\x04exit\r')
                        deadline=time.monotonic()+3
                        while time.monotonic()<deadline:guest.pump()
                    # An interrupted interactive check is allowed to leave a dirty
                    # disposable disk, never to execute cleanup in the host shell.
                    (args.output/'guest.log').write_bytes(guest.log)
                    if guest.prompt not in guest.log[-64:]:
                        if guest.process.poll() is None:guest.process.kill();guest.process.wait()
                        os.close(guest.master);guest=None
                if guest:
                    try:guest.close()
                    finally:(args.output/'guest.log').write_bytes(guest.log)
            finally:
                if peer:peer.close()
                server.terminate();server.wait(timeout=10);log.close()
if __name__=='__main__':main()
