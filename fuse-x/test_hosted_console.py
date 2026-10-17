#!/usr/bin/env python3
"""Use the real launcher to detach/reconnect and boot a remote-only kernel."""
import argparse
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import termios
import time
from session import connect, running
from test_support import check_root
from test_network import Services

HERE=Path(__file__).resolve().parent
LAUNCHER=HERE.parent/'scripts/run-fuse-x.sh'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    args=parser.parse_args()
    executable=args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='fx-console-',dir='/tmp') as temporary:
        work=Path(temporary).resolve()
        image=work/'root.img'
        (work/'fuse-x').symlink_to(executable)
        share=work/'Mac folder';share.mkdir()
        state=work/'session'
        subprocess.run(['node',str(HERE/'image.mjs'),'create',str(image),'--apps',str(executable.parent/'apps')],check=True)
        environment=dict(os.environ,FUSE_X_BUILD=str(work),RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='40')
        def launch(*arguments):
            result=subprocess.run([str(LAUNCHER),*arguments],capture_output=True,env=environment,timeout=20)
            assert result.returncode==0,result.stdout+result.stderr
            return result
        pending={}
        def receive(connection,marker):
            data=pending.setdefault(connection,bytearray());connection.settimeout(10)
            while marker not in data:
                chunk=connection.recv(8192)
                assert chunk,data
                data.extend(chunk)
            end=data.index(marker)+len(marker)
            result=bytes(data[:end]);del data[:end]
            return result
        def attach_console():
            connection=connect(state,b'A')
            received=receive(connection,b'fuse-x# ')
            assert received.startswith(b'OK\n'),received
            marker=('CONSOLE_READY_'+str(len(pending))).encode()
            connection.sendall(b'echo '+marker+b'\n')
            receive(connection,b'\r\n'+marker+b'\r\n');receive(connection,b'fuse-x# ')
            return connection
        try:
            launch('--run-only','--disk',str(image),'--share',str(share),'--detach')
            first=attach_console()
            first.sendall(b'echo broker-note > /mnt/host/note\n')
            receive(first,b'fuse-x# ')
            assert (share/'note').read_bytes()==b'broker-note\n'
            first.close();time.sleep(.15)
            # The public attach command must restore its own terminal on Ctrl-].
            master,slave=pty.openpty();saved=termios.tcgetattr(slave)
            client=subprocess.Popen([str(LAUNCHER),'--attach'],stdin=slave,stdout=slave,stderr=slave,env=environment)
            try:
                received=bytearray();deadline=time.monotonic()+10
                while b'fuse-x# ' not in received:
                    assert time.monotonic()<deadline,received
                    if select.select([master],[],[],.1)[0]:received.extend(os.read(master,8192))
                os.write(master,b'\x1d')
                assert client.wait(timeout=5)==0
                restored=termios.tcgetattr(slave)
                for settings in (saved,restored):settings[3]&=~getattr(termios,'PENDIN',0)
                assert saved==restored,'console detach did not restore terminal'
            finally:
                if client.poll() is None:client.terminate();client.wait(timeout=5)
                os.close(master);os.close(slave)
            time.sleep(.15)
            second=attach_console()
            second.sendall(b'exit\n')
            receive(second,b'shell stopped');receive(second,b'fuse-x# ')
            second.close()
            launch('--stop')
            assert not running(state)
            check_root(image)
            assert b'session stopped' in (state/'console.log').read_bytes()
            # Start with no local shell, and opt into the existing Telnet service.
            peer=Services(work/'nic')
            environment['RCC_NATIVE_NET']=str(work/'nic')
            try:
                launch('--run-only','--disk',str(image),'--no-share','--headless','--telnet')
                def until(condition):
                    deadline=time.monotonic()+15
                    while not condition():
                        assert time.monotonic()<deadline,(state/'console.log').read_bytes()
                        peer.pump()
                until(lambda:b'telnetd: listening' in (state/'console.log').read_bytes())
                assert b'fuse-x# ' not in (state/'console.log').read_bytes()
                remote=peer.connect(23,53000)
                until(lambda:remote.established)
                remote.queue(b'\xff\xfd\x01\xff\xfd\x03')
                until(lambda:b'fuzix# ' in remote.received)
                remote.queue(b'echo HEADLESS_REMOTE_OK\r\n')
                until(lambda:b'HEADLESS_REMOTE_OK\r\n' in remote.received)
                remote.queue(b'exit\r\n');until(lambda:remote.closed)
                assert running(state),'closing the last remote shell stopped the kernel'
                launch('--stop')
            finally:peer.close()
            check_root(image)
        finally:
            if running(state):
                subprocess.run([str(LAUNCHER),'--stop'],env=environment,capture_output=True,timeout=20)
    print('Fuse-X: launcher detach/reconnect, terminal restore, shell respawn, shared folder, headless Telnet and orderly stop passed')


if __name__=='__main__':main()
