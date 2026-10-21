#!/usr/bin/env python3
"""Real POSIX filesystem operations against an independent OpenSSH server."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import socket
import re
import subprocess
import sys
import tempfile
import time
from client import ROOT, Guest, Outbound

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deny-extensions',action='store_true')
    parser.add_argument('--target',choices=('native','x64'),default='native')
    parser.add_argument('--output',type=Path,default=ROOT/'build/ssh/mount-tests')
    args=parser.parse_args();
    if args.deny_extensions and args.target=='x64':parser.error('extension rejection fixture currently uses Fuse-X')
    args.output.mkdir(parents=True,exist_ok=True)
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(Path(__file__).with_name('mountcheck.c')),'--static','-o',str(args.output/'mountcheck.fxe')],check=True)
    artifacts=([ROOT/'build/ssh/sftp-mount.fxe',ROOT/'build/ssh/commander.fxe',ROOT/'fuse-x/build/fuse-x']
        if args.target=='native' else [ROOT/'build/ssh/x64/sftp-mount.rxe',ROOT/'build/ssh/x64-boot/normal/boot.img'])
    (args.output/'input.json').write_text(json.dumps(dict(target=args.target,deny_extensions=args.deny_extensions,
        artifacts=[dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in artifacts]),indent=2)+'\n')
    with tempfile.TemporaryDirectory(prefix='sftp-mount-',dir='/tmp') as temporary:
        work=Path(temporary).resolve();work.chmod(0o700)
        def run(cmd):subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        for name in ('host','user'):
            run(['ssh-keygen','-q','-t','ecdsa','-b','256','-m','PEM','-N','','-f',str(work/name)])
        run(['openssl','ec','-in',str(work/'user'),'-param_enc','named_curve','-out',str(work/'named')]);os.replace(work/'named',work/'user')
        (work/'authorized_keys').write_bytes((work/'user.pub').read_bytes());(work/'authorized_keys').chmod(0o600)
        remote=work/'remote';remote.mkdir();(remote/'original').write_bytes(bytes(range(256))*32)
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
Subsystem sftp internal-sftp{' -P posix-rename,fsync' if args.deny_extensions else ''}
LogLevel VERBOSE
''')
        pub=(work/'host.pub').read_text().split()
        files=[dict(name='/upload-me',text='Commander upload\n'),dict(name='/etc/termcap',text='ansi|vt100|xterm|ripple:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n'),dict(name='/root/.ssh/id_ecdsa',mode=0o100600,path=str(work/'user')),
            dict(name='/root/.ssh/known_hosts',mode=0o100600,text=f'[192.168.64.1]:2222 {pub[0]} {pub[1]}\n'),
            dict(name='/root/.ssh/config',mode=0o100600,text=f'Host fixture\n HostName 192.168.64.1\n User {user}\n Port 2222\n StrictHostKeyChecking yes\n BatchMode yes\n')]
        log=(args.output/'openssh.log').open('w');server=subprocess.Popen(['/usr/sbin/sshd','-D','-e','-f',str(config)],stdout=log,stderr=log)
        peer=guest=None
        try:
            until=time.monotonic()+5
            while True:
                if server.poll() is not None:raise AssertionError((args.output/'openssh.log').read_text())
                try:s=socket.create_connection(('127.0.0.1',port),.1);s.close();break
                except OSError:
                    if time.monotonic()>until:raise
                    time.sleep(.02)
            if args.target=='x64':
                from mount_x64 import run as run_x64
                run_x64(work,args.output,remote,port,files)
                return
            peer=Outbound(work/'nic',port)
            apps={'sftp-mount':ROOT/'build/ssh/sftp-mount.fxe','mountcheck':args.output/'mountcheck.fxe','commander':ROOT/'build/ssh/commander.fxe'}
            for name in ('mkdir','ls','kill','sleep'):apps[name]=ROOT/f'fuse-x/build/apps/{name}.fxe'
            guest=Guest(work,'native',apps,files,peer,blocks=65536)
            def command(cmd,status=0):
                text=guest.shell(cmd+'; echo MOUNT_STATUS:$?',180)
                (args.output/'guest.log').write_bytes(guest.log)
                assert f'MOUNT_STATUS:{status}\n' in text,text
                return text
            command('mkdir /mnt; mkdir /mnt/remote')
            command(f'sftp-mount --background fixture:{remote} /mnt/remote')
            if args.deny_extensions:
                (remote/'original').write_bytes(b'KEEP')
                text=command('mountcheck --unsupported');assert 'SFTP_UNSUPPORTED_OK' in text,text
                assert (remote/'original').read_bytes()==b'KEEP'
                command('sftp-mount --unmount 0')
                guest.close();guest=None
                print('PASS server-refused remote fsync/atomic-rename extensions fail explicitly and preserve the original',flush=True)
                return
            text=command('mountcheck');assert 'SFTP_MOUNT_OK' in text,text
            assert (remote/'original').read_bytes()==b'after'
            assert not (remote/'moved').exists() and not (remote/'replacement').exists()
            # A busy cwd cannot be detached; after leaving, detach is clean.
            command('cd /mnt/remote; sftp-mount --unmount 0',1)
            command('cd /; sftp-mount --unmount 0')
            command('sftp-mount --status')
            # Exercise rendered cells: curses may update a title in several
            # cursor-addressed pieces rather than retransmitting its text.
            sys.path.insert(0,str(ROOT/'rfl/tools'))
            from test_os import Screen
            screen=Screen();screen_at=0
            def visible(text):
                nonlocal screen_at
                until=time.monotonic()+30
                while True:
                    screen.feed(bytes(guest.log[screen_at:]));screen_at=len(guest.log)
                    if text in screen.text():
                        guest.at=len(guest.log);return
                    guest.pump()
                    if guest.process.poll() is not None or time.monotonic()>until:
                        raise AssertionError(text+'\n'+screen.text())
            # Exercise Commander's actual dialogs and ordinary POSIX panels.
            os.write(guest.master,b'commander / /\r')
            visible('RFL Commander')
            os.write(guest.master,b's');visible('SFTP connections')
            os.write(guest.master,b'\r');visible('SSH profile or user@host:/directory')
            os.write(guest.master,f'fixture:{remote}\r'.encode())
            visible('Mount directory (empty, under /mnt)')
            os.write(guest.master,b'\r');visible('SFTP connected; ordinary copy')
            os.write(guest.master,b'\x1b[B');visible('Selected: /mnt/remote/original')
            os.write(guest.master,b'\x1b[15~');visible('Copy to')
            os.write(guest.master,b'\x15/copy-from-remote\r');visible('Copied 5 bytes')
            os.write(guest.master,b'\t\x1b[F');visible('Selected: /upload-me')
            os.write(guest.master,b'\x1b[15~');visible('Copy to')
            os.write(guest.master,b'\r');visible('Copied 17 bytes')
            assert (remote/'upload-me').read_bytes()==b'Commander upload\n'
            os.write(guest.master,b's');visible('SFTP connections')
            os.write(guest.master,b'\x1b[B\r');visible('Select connection to disconnect')
            os.write(guest.master,b'\r');visible('Disconnect /mnt/remote?')
            os.write(guest.master,b'y');visible('SFTP disconnected')
            os.write(guest.master,b'q');guest.read(guest.prompt)
            print('PASS Commander connect dialog, SSH profile, mounted panel and disconnect dialog',flush=True)
            # Killing an idle service must fail subsequent filesystem calls
            # explicitly and leave the local shell usable and mount removable.
            command(f'sftp-mount --background fixture:{remote} /mnt/remote')
            (remote/'badnames').mkdir();(remote/'badnames'/('x'*31)).write_text('unrepresentable')
            (remote/('n'*30)).write_text('KEEP_PREFIX')
            text=command('mountcheck --names');assert 'SFTP_NAMES_OK' in text,text
            assert (remote/('n'*30)).read_text()=='KEEP_PREFIX'
            # Separate mounts may reuse the same inode numbers internally;
            # their kernel devices/mailboxes must remain independent.
            for index in range(1,4):
                command(f'mkdir /mnt/remote{index}')
                command(f'sftp-mount --background fixture:{remote} /mnt/remote{index}')
                assert '\nafter\n' in command(f'cat /mnt/remote{index}/original; echo')
            command('mkdir /mnt/overflow')
            command(f'sftp-mount --background fixture:{remote} /mnt/overflow',1)
            for index in range(3,0,-1):command(f'sftp-mount --unmount {index}')
            status=command('sftp-mount --status')
            pid=re.search(r'PID (\d+)',status).group(1)
            command('kill -9 '+pid)
            text=command('cat /mnt/remote/original',1)
            text=command('sftp-mount --status');assert 'disconnected' in text,text
            command('sftp-mount --unmount 0')
            command('cat /etc/resolv.conf')
            guest.close();guest=None
            print('PASS real SFTP mount: binary I/O, create/append/truncate, atomic overwrite, directory rename, metadata/listing, concurrent clients, busy and clean unmount',flush=True)
        finally:
            if guest:
                (args.output/'guest.log').write_bytes(guest.log)
                if guest.process.poll() is None:guest.process.kill();guest.process.wait()
                os.close(guest.master)
            if peer:peer.close()
            server.terminate();server.wait(timeout=10);log.close()
if __name__=='__main__':main()
