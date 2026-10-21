#!/usr/bin/env python3
"""Real OpenSSH -> independent Ethernet peer -> guest SSH server acceptance."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
sys.path.insert(0,str(ROOT/'ports/fuzix/tools'))
from guest import Guest
from network_peer import Peer


class Relay(Peer):
    """Only forwards octets; SSH/authentication/crypto execute inside the guest."""
    def __init__(self,path):
        super().__init__(path)
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(8)
        self.listener.setblocking(False);self.port=self.listener.getsockname()[1]
        self.clients=[];self.local=50000

    def pump(self):
        super().pump()
        try:
            sock,_=self.listener.accept();sock.setblocking(False)
            self.local+=1
            self.clients.append(dict(socket=sock,stream=self.connect(2222,self.local),at=0,eof=False,closed=False))
        except BlockingIOError:pass
        for c in self.clients:
            if c['closed']:continue
            sock,s=c['socket'],c['stream']
            if len(s.pending)<65536 and not c['eof']:
                try:
                    data=sock.recv(16384)
                    if data:s.queue(data)
                    else:c['eof']=True;s.closing=True
                except BlockingIOError:pass
                except ConnectionResetError:c['eof']=True;s.closing=True
            if c['at']<len(s.received):
                try:c['at']+=sock.send(s.received[c['at']:])
                except BlockingIOError:pass
                except (BrokenPipeError,ConnectionResetError):c['eof']=True;s.closing=True;c['at']=len(s.received)
            if (s.fin or s.reset) and c['at']==len(s.received):
                sock.close();c['closed']=True

    def close(self):
        for c in self.clients:c['socket'].close()
        self.listener.close();super().close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,default=ROOT/'build/ssh/sshd.fxe')
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    parser.add_argument('--output',type=Path,default=ROOT/'build/ssh/tests')
    cases=parser.add_mutually_exclusive_group()
    cases.add_argument('--files-only',action='store_true')
    cases.add_argument('--terminal-only',action='store_true')
    cases.add_argument('--list-only',action='store_true')
    cases.add_argument('--key-types-only',action='store_true')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    args.executable=args.executable.resolve()
    if not args.executable.is_file():parser.error('build the SSH executable first')
    with (args.executable.parent/(args.target+'.lock')).open('a') as build_lock, \
            tempfile.TemporaryDirectory(prefix='ssh-',dir='/tmp') as directory:
        # Do not copy an older executable while app.py is still linking its
        # replacement. Hold a shared build lock through this acceptance run.
        fcntl.flock(build_lock,fcntl.LOCK_SH)
        (args.output/'input.json').write_text(json.dumps(dict(
            target=args.target,executable=str(args.executable),
            sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest()),indent=2)+'\n')
        work=Path(directory)
        def run(command):subprocess.run(command,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        for name,bits in (('host',256),('user',256),('wrong',256),('user384',384),('user521',521)):
            run(['ssh-keygen','-q','-t','ecdsa','-b',str(bits),'-m','PEM','-N','','-f',str(work/name)])
        for name,kind,bits in (('userrsa','rsa',2048),('userrsa4096','rsa',4096),('usered','ed25519',None)):
            command=['ssh-keygen','-q','-t',kind,'-N','','-f',str(work/name)]
            if bits:command+=['-b',str(bits)]
            run(command)
        run(['openssl','ec','-in',str(work/'host'),'-param_enc','named_curve','-outform','DER','-out',str(work/'host.der')])
        (work/'authorized_keys').write_bytes(b''.join((work/name).read_bytes() for name in ('user.pub','user384.pub','user521.pub','userrsa.pub','userrsa4096.pub','usered.pub')))
        (work/'known_hosts').write_text('riplix-fixture '+(work/'host.pub').read_text())
        expected=bytes(range(256))*384+b'Riplix SSH binary transfer\0\xff\n'
        (work/'binary').write_bytes(expected)
        peer=Relay(work/'nic')
        (work/'config').write_text(f'''Host fixture
 HostName 127.0.0.1
 Port {peer.port}
 User root
 HostKeyAlias riplix-fixture
 UserKnownHostsFile {work/'known_hosts'}
 GlobalKnownHostsFile /dev/null
 StrictHostKeyChecking yes
 IdentitiesOnly yes
 BatchMode yes
 IdentityFile {work/'user'}
 ConnectTimeout 30
 LogLevel ERROR
''')
        native=args.target=='native'
        suffix='.fxe' if native else '.rxe'
        sdk=ROOT/('fuse-x/build/apps' if native else 'build/fuzix')
        apps={'sshd':args.executable,'stty':sdk/('stty'+suffix)}
        guest=Guest(work,args.target,apps,[
            dict(name='/etc/ssh/hostkey.der',mode=0o100600,path=str(work/'host.der')),
            dict(name='/etc/ssh/insecure.der',mode=0o100644,path=str(work/'host.der')),
            dict(name='/root/.ssh/authorized_keys',mode=0o100600,path=str(work/'authorized_keys')),
            dict(name='/root/.ssh/options',mode=0o100600,text='no-pty '+(work/'user.pub').read_text()),
            dict(name='/binary',path=str(work/'binary')),
            *([dict(name='/sftp-upload',path=str(work/'binary'))] if args.list_only else []),
            *[dict(name=f'/dev/{kind}{i:x}',mode=0o20600,device=base+i)
              for i in range(16) for kind,base in [('ptyp',0x300),('ttyp',0x202)]],
        ],peer,blocks=65536,code_memory=None if native else 8388608)
        base=['ssh','-F','/dev/null','-p',str(peer.port),'-o','HostKeyAlias=riplix-fixture',
              '-o','UserKnownHostsFile='+str(work/'known_hosts'),'-o','GlobalKnownHostsFile=/dev/null',
              '-o','StrictHostKeyChecking=yes','-o','IdentitiesOnly=yes','-o','BatchMode=yes',
              '-o','ConnectTimeout=30','-o','LogLevel=DEBUG1','-i',str(work/'user'),'root@127.0.0.1']
        def ssh(command,input=b'',key=None,extra=(),seconds=120):
            argv=base[:]
            if key:argv[argv.index('-i')+1]=str(work/key)
            argv=argv[:-1]+list(extra)+[argv[-1],command]
            with tempfile.TemporaryFile() as incoming,tempfile.TemporaryFile() as out,tempfile.TemporaryFile() as err:
                incoming.write(input);incoming.seek(0)
                process=subprocess.Popen(argv,stdin=incoming,stdout=out,stderr=err)
                until=time.monotonic()+seconds
                try:
                    while process.poll() is None:
                        guest.pump()
                        if time.monotonic()>until:raise AssertionError('OpenSSH timed out')
                    out.seek(0);err.seek(0)
                    result=(process.returncode,out.read(),err.read())
                    (args.output/'client.log').write_bytes(result[2])
                    return result
                finally:
                    if process.poll() is None:process.kill();process.wait()
        def file_tool(command,seconds=120):
            with tempfile.TemporaryFile() as out,tempfile.TemporaryFile() as err:
                process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err)
                until=time.monotonic()+seconds
                try:
                    while process.poll() is None:
                        guest.pump()
                        if time.monotonic()>until:raise AssertionError('File transfer timed out')
                    out.seek(0);err.seek(0);stdout,stderr=out.read(),err.read()
                    (args.output/'file-client.log').write_bytes(stdout+stderr)
                    assert process.returncode==0,(command,process.returncode,stdout,stderr)
                    return stdout
                finally:
                    if process.poll() is None:process.kill();process.wait()
        def interactive():
            master,slave=pty.openpty()
            fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',37,91,0,0))
            argv=base[:-1]+['-tt',base[-1]]
            process=subprocess.Popen(argv,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
            os.close(slave);output=bytearray();at=0
            def until(marker,seconds=120):
                nonlocal at
                deadline=time.monotonic()+seconds
                while marker not in output[at:]:
                    guest.pump()
                    if select.select([master],[],[],0)[0]:
                        try:output.extend(os.read(master,65536).replace(b'\r',b''))
                        except OSError:pass
                    (args.output/'interactive.log').write_bytes(output)
                    if process.poll() is not None or time.monotonic()>deadline:
                        raise AssertionError(('interactive SSH timed out',bytes(output[-6000:])))
                at=output.index(marker,at)+len(marker)
            try:
                until(b'Entering interactive session.')
                os.write(master,b'stty -a; echo TERMINAL_STARTED\r')
                until(b'\nTERMINAL_STARTED\n')
                assert b'37 rows 91 columns' in output,bytes(output)
                fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',41,103,0,0))
                process.send_signal(signal.SIGWINCH)
                deadline=time.monotonic()+.2
                while time.monotonic()<deadline:guest.pump()
                os.write(master,b'stty -a; echo TERMINAL_RESIZED\r')
                until(b'\nTERMINAL_RESIZED\n')
                assert b'41 rows 103 columns' in output,bytes(output)
                os.write(master,b'cat\r')
                until(b'cat\n')
                os.write(master,b'CAT_IS_READING\r')
                until(b'CAT_IS_READING\n')  # terminal echo
                until(b'CAT_IS_READING\n')  # cat has actually read the line
                os.write(master,b'\x03')
                until(b'# ')
                os.write(master,b'echo INTERRUPT_SURVIVED\r')
                until(b'\nINTERRUPT_SURVIVED\n')
                until(b'# ')
                os.write(master,b'exit\r')
                deadline=time.monotonic()+20
                while process.poll() is None:
                    guest.pump()
                    if select.select([master],[],[],0)[0]:
                        try:output.extend(os.read(master,65536).replace(b'\r',b''))
                        except OSError:pass
                    if time.monotonic()>deadline:raise AssertionError('interactive SSH did not exit')
                assert process.returncode==0,process.returncode
                print('PASS interactive shell, PTY resize and Ctrl-C isolation',flush=True)
            finally:
                if process.poll() is None:process.kill();process.wait()
                os.close(master)
                (args.output/'interactive.log').write_bytes(output)
        try:
            if not args.files_only and not args.terminal_only and not args.list_only and not args.key_types_only:
                text=guest.shell('sshd --host-key /etc/ssh/insecure.der; echo DENIED:$?')
                assert 'DENIED:1' in text,text
                text=guest.shell('sshd --authorized-keys /root/.ssh/options; echo DENIED:$?')
                assert 'DENIED:1' in text,text
                text=guest.shell('sshd --keygen /etc/ssh/generated.der; echo GENERATED:$?')
                assert 'GENERATED:0' in text,text
                text=guest.shell('sshd --keygen /etc/ssh/generated.der; echo EXISTS:$?')
                assert 'EXISTS:1' in text,text
                print('PASS guest host-key generation, overwrite refusal and credential policy',flush=True)
            text=guest.shell('SVC_READY_FD=3 sshd -p 2222 3>/ready &')
            if 'sshd: listening on port 2222' not in text:guest.read(b'sshd: listening on port 2222',30)
            text=guest.shell('cat /ready; echo READY_RECEIVED')
            assert 'RREADY_RECEIVED\n' in text,text
            print('PASS service readiness notification after listen',flush=True)
            if args.key_types_only:
                for bits,key in ((256,'user'),(384,'user384'),(521,'user521'),(2048,'userrsa'),(4096,'userrsa4096'),('Ed25519','usered')):
                    status,out,err=ssh('echo KEY_TYPE_OK',key=key)
                    assert status==0 and out==b'KEY_TYPE_OK\n',(bits,status,out,err)
                print('PASS ECDSA 256/384/521, RSA 2048/4096 SHA2 and Ed25519 authorized-key logins',flush=True)
                return
            if args.terminal_only:
                interactive()
                return
            if args.list_only:
                (work/'sftp.batch').write_text('ls /\nquit\n')
                file_tool(['sftp','-F',str(work/'config'),'-b',str(work/'sftp.batch'),'fixture'])
                print('PASS SFTP directory listing',flush=True)
                return
            if not args.files_only:
                status,out,err=ssh('echo SSH_REAL_COMMAND; exit 7')
                assert status==7 and out==b'SSH_REAL_COMMAND\n',(status,out,err)
                print('PASS OpenSSH authenticated remote command and exit status',flush=True)
                status,out,err=ssh('echo MUST_NOT_RUN',key='wrong')
                assert status==255 and b'MUST_NOT_RUN' not in out,(status,out,err)
                print('PASS wrong public key rejected',flush=True)
                status,out,err=ssh('cat /etc/ssh/generated.der')
                assert status==0,(status,out,err)
                (work/'generated.der').write_bytes(out)
                run(['openssl','ec','-inform','DER','-in',str(work/'generated.der'),'-check','-noout'])
                before=out
                status,out,err=ssh('sshd --keygen /etc/ssh/generated.der; cat /etc/ssh/generated.der')
                assert status==0 and out==before,(status,out,err)
                print('PASS generated private key validated independently and retained unchanged',flush=True)
                status,out,err=ssh('cat /binary')
                assert status==0 and out==expected,(status,len(out),err)
                status,out,err=ssh('cat > /upload',input=expected)
                assert status==0,(status,out,err)
                status,out,err=ssh('cat /upload')
                assert status==0 and out==expected,(status,len(out),err)
                print('PASS binary download/upload beyond signed C16 I/O boundary',flush=True)
                status,out,err=ssh('echo INPUT_CLOSED; exit 9',input=expected)
                assert status==9 and out==b'INPUT_CLOSED\n',(status,out,err)
                status,out,err=ssh('echo ERROR_STREAM >&2; exit 3')
                assert status==3 and b'\nERROR_STREAM\n' in err.replace(b'\r',b'') and not out,(status,out,err)
                print('PASS closed command stdin, separate stderr and exit status',flush=True)
                status,out,err=ssh('cat /binary',extra=('-o','RekeyLimit=16K'))
                assert status==0 and out==expected,(status,len(out),err)
                assert err.count(b'SSH2_MSG_KEXINIT sent')>=2,err
                print('PASS rekey during binary transfer',flush=True)
                status,out,err=ssh('echo PTY_OK; stty -a',extra=('-tt',))
                assert status==0 and b'PTY_OK' in out and b'rows' in out,(status,out,err)
                print('PASS PTY allocation and terminal command',flush=True)
                interactive()
                for index in range(6):
                    status,out,err=ssh('echo REUSED')
                    assert status==0 and out==b'REUSED\n',(index,status,out,err)
                print('PASS repeated connection/session reuse',flush=True)
            file_tool(['scp','-F',str(work/'config'),'-q',str(work/'binary'),'fixture:/sftp-upload'])
            file_tool(['scp','-F',str(work/'config'),'-q','fixture:/sftp-upload',str(work/'scp-download')])
            assert (work/'scp-download').read_bytes()==expected
            print('PASS ordinary OpenSSH scp upload/download over SFTP',flush=True)
            tree=work/'tree';(tree/'nested').mkdir(parents=True);(tree/'empty').mkdir()
            (tree/'nested/data').write_bytes(expected);(tree/'a b').write_text('spaces\n')
            file_tool(['scp','-r','-F',str(work/'config'),'-q',str(tree),'fixture:/server-tree'])
            file_tool(['scp','-r','-F',str(work/'config'),'-q','fixture:/server-tree',str(work/'server-roundtrip')])
            assert (work/'server-roundtrip/nested/data').read_bytes()==expected
            assert (work/'server-roundtrip/empty').is_dir() and (work/'server-roundtrip/a b').read_text()=='spaces\n'
            print('PASS recursive OpenSSH scp upload/download with nested and empty directories',flush=True)
            (work/'sftp.batch').write_text(f'''pwd
ls /
mkdir /sftp-dir
put {work/'binary'} /sftp-dir/original
rename /sftp-dir/original /sftp-dir/renamed
chmod 600 /sftp-dir/renamed
get /sftp-dir/renamed {work/'sftp-download'}
rm /sftp-dir/renamed
rmdir /sftp-dir
quit
''')
            file_tool(['sftp','-F',str(work/'config'),'-b',str(work/'sftp.batch'),'fixture'])
            assert (work/'sftp-download').read_bytes()==expected
            print('PASS SFTP listing, mkdir, upload, rename, chmod, download and removal',flush=True)
        finally:
            try:guest.close()
            finally:
                peer.close()
                (args.output/'guest.log').write_bytes(guest.log)
        print('PASS clean guest shutdown and filesystem validation',flush=True)


if __name__=='__main__':main()
