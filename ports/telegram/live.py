#!/usr/bin/env python3
"""Private live-test launcher. Only disk/terminal control; Telegram runs in guest C/RFL."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import tty

ROOT=Path(__file__).resolve().parents[2]
PRIVATE=ROOT/'build/telegram-private'
CONTROL=PRIVATE/'control.sock'
sys.path.insert(0,str(ROOT/'libraries/tests'))
from test_qr import screen,write_png


def request(command):
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    connection.settimeout(30)
    connection.connect(str(CONTROL))
    connection.sendall(json.dumps(command).encode()+b'\n')
    data=b''
    while b'\n' not in data:
        part=connection.recv(4096)
        if not part:raise RuntimeError('Private guest control connection closed')
        data+=part
    connection.close()
    return json.loads(data.split(b'\n')[0])


def image(config):
    manifest=[dict(name='/init',mode=0o100755,text='FXN1:init:1\n')]
    for name in ('sh','echo','cat','telegram'):
        manifest.append(dict(name='/bin/'+name,mode=0o100755,path=str(ROOT/'fuse-x/build/apps'/f'{name}.fxe')))
    for name,device,mode in [('tty',0x200,0o20666),('console',0x201,0o20600),('null',0x100,0o20666),('urandom',0x104,0o20444),('crypto',0x105,0o20600)]:
        manifest.append(dict(name='/dev/'+name,mode=mode,device=device))
    manifest += [dict(name='/etc/telegram.conf',mode=0o100600,path=str(config)),
                 dict(name='/etc/termcap',text='ansi|vt100|xterm:co#80:li#30:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:\n'),
                 dict(name='/etc/resolv.conf',text='nameserver 192.168.64.1\n')]
    manifest_path=PRIVATE/'manifest.json'
    manifest_path.write_text(json.dumps(manifest));manifest_path.chmod(0o600)
    builder=r'''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {openImage,writeImage} from './ports/fuzix/tools/image.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
const [manifest,disk]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest)).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.from(f.text||'')}));
let image;
if(fs.existsSync(disk)) {
    const original=openImage(disk), checked=checkRoot(original,true);
    if(checked.issues.length) {
        const backup=disk+'.recovery-'+Date.now();
        writeImage(backup,original,true);fs.chmodSync(backup,0o600);
        if(checkRoot(checked.image).issues.length)throw Error('Private filesystem repair failed');
    }
    image=installRoot(checked.image,files);
} else image=rootImage(files,32768,256);
const temporary=disk+'.new';if(fs.existsSync(temporary))fs.unlinkSync(temporary);
writeImage(temporary,image,true);fs.chmodSync(temporary,0o600);fs.renameSync(temporary,disk);
'''
    subprocess.run(['node','--input-type=module','-e',builder,str(manifest_path),str(PRIVATE/'guest.img')],cwd=ROOT,check=True)


def serve(config):
    PRIVATE.mkdir(parents=True,exist_ok=True,mode=0o700);PRIVATE.chmod(0o700)
    lock=(PRIVATE/'launch.lock').open('w');os.chmod(lock.name,0o600)
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    with (PRIVATE/'guest.img.lock').open('a') as disk_lock:
        os.chmod(disk_lock.name,0o600)
        fcntl.flock(disk_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        image(config)
    helper=ROOT/'build/qr-test/qr-check'
    if not helper.exists():
        subprocess.run([sys.executable,str(ROOT/'libraries/tests/test_qr.py'),'--target','host'],cwd=ROOT,check=True)
    CONTROL.unlink(missing_ok=True)
    listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    listener.bind(str(CONTROL));os.chmod(CONTROL,0o600);listener.listen(4);listener.setblocking(False)
    master,slave=pty.openpty();tty.setraw(slave)
    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,80,0,0))
    child=subprocess.Popen([str(ROOT/'fuse-x/build/fuse-x'),'--run',str(PRIVATE/'guest.img')],stdin=slave,stdout=slave,stderr=slave,
        env=dict(os.environ,TERM='xterm',RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='shared',RCC_NATIVE_NET_EAGER='1',RCC_NATIVE_TIMEOUT='3600'),start_new_session=True)
    os.close(slave)
    (PRIVATE/'pid').write_text(str(os.getpid()));os.chmod(PRIVATE/'pid',0o600)
    capture=(PRIVATE/'terminal.ansi').open('wb');os.chmod(capture.name,0o600)
    log=bytearray();clients={};started=False;stopping=False;stop_at=0;shutdown=False;stop_log=0
    def stop(signum=None,frame=None):
        nonlocal stopping,stop_at,shutdown,stop_log
        if not stopping:
            stopping=True;stop_at=time.monotonic();stop_log=len(log)
            if bytes(log).rstrip().endswith(b'fuse-x#'):
                os.write(master,b'shutdown\r');shutdown=True
            else:os.write(master,b'\x1b[21~')
    signal.signal(signal.SIGTERM,stop)
    signal.signal(signal.SIGINT,stop)
    try:
        while child.poll() is None:
            readable,_,_=select.select([master,listener,*clients],[],[],.05)
            for source in readable:
                if source==master:
                    try:data=os.read(master,65536)
                    except OSError:data=b''
                    if not data:continue
                    log.extend(data);capture.write(data);capture.flush()
                    for connection,state in list(clients.items()):
                        if state is None:
                            try:connection.sendall(data)
                            except OSError:connection.close();del clients[connection]
                    if not started and b'fuse-x# ' in log:
                        os.write(master,b'telegram\r');started=True
                    if stopping and not shutdown and b'fuse-x# ' in log[stop_log:]:
                        os.write(master,b'shutdown\r');shutdown=True
                elif source==listener:
                    connection,_=listener.accept();connection.setblocking(False);clients[connection]=bytearray()
                else:
                    connection=source
                    try:data=connection.recv(4096)
                    except OSError:data=b''
                    if not data:connection.close();del clients[connection];continue
                    if clients[connection] is None:os.write(master,data);continue
                    clients[connection].extend(data)
                    if len(clients[connection])>2048:connection.close();del clients[connection];continue
                    if b'\n' not in clients[connection]:continue
                    try:
                        command=json.loads(clients[connection].split(b'\n')[0])
                        action=command['command']
                        cells=screen(helper,bytes(log))
                        rows=[''.join(chr(ch) if 32<=ch<127 else ' ' for ch,_ in cells[y*80:(y+1)*80]).rstrip() for y in range(30)]
                        result={'ok':True}
                        if action=='status':result['screen']=rows
                        elif action=='snapshot':
                            path=PRIVATE/('screen-'+str(time.time_ns())+'.png');write_png(path,cells);path.chmod(0o600);result['path']=str(path)
                        elif action=='password_file':
                            if not rows[2].strip().startswith('Enter your Telegram 2FA password'):raise ValueError('Guest is not at its 2FA password prompt')
                            path=Path(command['path']);metadata=path.lstat()
                            import stat
                            if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode&0o077:raise ValueError('Password file must be a private regular file (mode 0600)')
                            value=path.read_bytes()
                            if value.endswith(b'\n'):value=value[:-1]
                            if value.endswith(b'\r'):value=value[:-1]
                            try:value.decode('utf-8')
                            except UnicodeDecodeError:raise ValueError('Password file must contain valid UTF-8') from None
                            if len(value)>512 or any(byte<32 or byte==127 for byte in value):raise ValueError('Password must be one UTF-8 line, at most 512 octets')
                            os.write(master,value+b'\r');del value
                        elif action=='resize':
                            fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',int(command['rows']),int(command['columns']),0,0))
                        elif action=='stop':stop()
                        elif action=='attach':
                            connection.settimeout(2)
                            connection.sendall(bytes(log));connection.setblocking(False)
                            clients[connection]=None;continue
                        else:raise ValueError('Unknown control command')
                    except Exception as error:result={'ok':False,'error':str(error)}
                    try:
                        connection.settimeout(2)
                        connection.sendall(json.dumps(result).encode()+b'\n')
                    except OSError:pass
                    finally:connection.close();del clients[connection]
            if stopping and time.monotonic()-stop_at>15:
                child.kill();break
        child.wait(timeout=5)
    finally:
        if child.poll() is None:child.kill();child.wait()
        for connection in clients:connection.close()
        listener.close();CONTROL.unlink(missing_ok=True);os.close(master);capture.close()


def attach():
    size=shutil.get_terminal_size()
    if size.columns<80 or size.lines<30:raise SystemExit('Resize this terminal to at least 80x30 before attaching.')
    request({'command':'resize','rows':30,'columns':80})
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.connect(str(CONTROL))
    connection.sendall(b'{"command":"attach"}\n')
    original=termios.tcgetattr(0);tty.setraw(0)
    try:
        while True:
            readable,_,_=select.select([0,connection],[],[])
            if 0 in readable:
                data=os.read(0,4096)
                if not data:break
                connection.sendall(data)
            if connection in readable:
                data=connection.recv(65536)
                if not data:break
                os.write(1,data)
    finally:termios.tcsetattr(0,termios.TCSANOW,original);connection.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path)
    parser.add_argument('--background',action='store_true')
    parser.add_argument('--serve',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--attach',action='store_true')
    parser.add_argument('--snapshot',action='store_true')
    parser.add_argument('--status',action='store_true')
    parser.add_argument('--stop',action='store_true')
    parser.add_argument('--password-file',type=Path)
    args=parser.parse_args()
    if args.attach:attach();return
    if args.status or args.snapshot or args.stop or args.password_file:
        result=request({'command':'password_file','path':str(args.password_file.resolve())} if args.password_file else
                       {'command':'snapshot' if args.snapshot else 'stop' if args.stop else 'status'})
        if 'screen' in result:print('\n'.join(result['screen']))
        else:print(json.dumps(result))
        if not result['ok']:raise SystemExit(1)
        return
    if not args.config:parser.error('--config is required to start the private guest')
    if args.serve:serve(args.config.resolve());return
    PRIVATE.mkdir(mode=0o700,parents=True,exist_ok=True);PRIVATE.chmod(0o700)
    output=(PRIVATE/'launcher.log').open('ab');os.chmod(output.name,0o600)
    subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--serve','--config',str(args.config.resolve())],cwd=ROOT,
                     stdin=subprocess.DEVNULL,stdout=output,stderr=output,start_new_session=True)
    until=time.monotonic()+20
    while time.monotonic()<until:
        if CONTROL.exists():
            print('Private Telegram guest started. Use --status, --snapshot or --attach.');return
        time.sleep(.1)
    raise SystemExit('Guest did not start; inspect the private launcher.log')


if __name__=='__main__':main()
