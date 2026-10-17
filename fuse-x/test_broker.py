#!/usr/bin/env python3
"""Exercise persistent console ownership, replay and orderly stop without a kernel."""
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import sys
import tempfile
import termios
import time
from session import connect, running

HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix='fx-broker-', dir='/tmp') as temporary:
        state = Path(temporary) / 'session'
        probe = ("import os,signal,sys\n"
                 "signal.signal(signal.SIGTERM,lambda *args: (print('clean-stop',flush=True),sys.exit(0)))\n"
                 "with open('/dev/tty','rb+',buffering=0) as tty:\n"
                 " tty.write(b'Password: '); assert tty.read(1)==b'x'\n"
                 "print('ready',flush=True)\n"
                 "for line in sys.stdin:\n"
                 " if line.strip()=='flood': print('F'*(9*1024*1024)+'flood-end',flush=True)\n"
                 " else: print('reply:'+line.strip(),flush=True)\n")
        server = subprocess.Popen([sys.executable, str(HERE/'session.py'), 'serve', '--state', str(state),
                                   '--', sys.executable, '-u', '-c', probe],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def receive(connection, marker):
            data = bytearray()
            connection.settimeout(5)
            while marker not in data:
                chunk = connection.recv(4096)
                assert chunk, data
                data.extend(chunk)
            return bytes(data)
        try:
            deadline = time.monotonic()+5
            while not running(state):
                assert server.poll() is None, server.stdout.read()
                assert time.monotonic()<deadline
                time.sleep(.05)
            first = connect(state,b'A')
            received=receive(first,b'Password: ')
            assert received.startswith(b'OK\n'), received
            first.sendall(b'x')
            receive(first,b'ready')
            first.sendall(b'one\n')
            receive(first,b'reply:one')
            with connect(state,b'A') as duplicate:
                assert receive(duplicate,b'\n').startswith(b'ERROR console already attached')
            first.close()
            time.sleep(.15)
            second=connect(state,b'A')
            replay=receive(second,b'reply:one')
            assert replay.startswith(b'OK\n'),replay
            second.sendall(b'two\n')
            receive(second,b'reply:two')
            second.sendall(b'flood\n');second.close()
            deadline=time.monotonic()+10
            while not (state/'console.log.1').exists() or b'flood-end' not in (state/'console.log').read_bytes():
                assert time.monotonic()<deadline
                time.sleep(.05)
            assert (state/'console.log.1').stat().st_size<=8*1024*1024
            assert (state/'console.log').stat().st_size<=8*1024*1024
            second=connect(state,b'A')
            assert len(receive(second,b'flood-end'))<=65536+3
            stop=subprocess.run([sys.executable,str(HERE/'session.py'),'stop','--state',str(state)],
                                capture_output=True,timeout=10)
            assert stop.returncode==0,stop.stderr
            assert b'clean-stop' in receive(second,b'clean-stop')
            second.close()
            assert server.wait(timeout=5)==0,server.stdout.read()
            assert not running(state)
            assert not (state/'console.sock').exists()
            assert b'clean-stop' in (state/'console.log').read_bytes()
        finally:
            if server.poll() is None:
                server.terminate()
                try: server.wait(timeout=5)
                except subprocess.TimeoutExpired: server.kill();server.wait()
        if sys.platform=='darwin':
            # Background vmnet startup must allow sudo to use its controlling
            # terminal before the foreground launcher detaches automatically.
            state=Path(temporary)/'authorization'
            boot_probe=probe.replace("print('ready',flush=True)",
                                     "print('Fuse-X: native /init running as PID 1',flush=True)")
            master,slave=pty.openpty();saved=termios.tcgetattr(slave)
            launcher=subprocess.Popen([sys.executable,str(HERE/'session.py'),'start','--state',str(state),
                '--detach','--',sys.executable,'-u','-c',boot_probe],stdin=slave,stdout=slave,stderr=slave,
                env=dict(os.environ,RCC_NATIVE_NET='shared'))
            try:
                data=bytearray();deadline=time.monotonic()+10
                while b'Password: ' not in data:
                    assert time.monotonic()<deadline,data
                    if select.select([master],[],[],.1)[0]:data.extend(os.read(master,4096))
                os.write(master,b'x')
                assert launcher.wait(timeout=5)==0
                assert running(state),'authorization completion stopped the background session'
                restored=termios.tcgetattr(slave)
                for attributes in (saved,restored):attributes[3]&=~getattr(termios,'PENDIN',0)
                assert saved==restored,'authorization console did not restore terminal'
            finally:
                if launcher.poll() is None:launcher.terminate();launcher.wait(timeout=5)
                os.close(master);os.close(slave)
                if running(state):
                    subprocess.run([sys.executable,str(HERE/'session.py'),'stop','--state',str(state)],
                                   check=True,timeout=10)
    print('Fuse-X: console attach/detach, replay, exclusive ownership, startup authorization and orderly stop passed')


if __name__=='__main__': main()
