#!/usr/bin/env python3
"""Exercise persistent init, CPU-bound programs, signals and host shutdown."""
import argparse
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time
from test_support import check_root

HERE=Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--compiler',type=Path,required=True)
    args=parser.parse_args()
    executable=args.executable.resolve()
    environment=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='45')
    with tempfile.TemporaryDirectory(prefix='fx-life-',dir='/tmp') as temporary:
        work=Path(temporary)
        image=work/'root.img'
        subprocess.run(['node',str(HERE/'image.mjs'),'create',str(image),'--apps',str(executable.parent/'apps')],check=True)
        program=work/'fairness.fxe'
        subprocess.run(['python3',str(HERE/'cc.py'),str(HERE/'examples/fairness.c'),
                        '--compiler',str(args.compiler.resolve()),'--build',str(executable.parent),
                        '-o',str(program)],check=True)
        child=subprocess.Popen([str(executable),'--run',str(image),'--share',str(work)],
                               stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment)
        received=bytearray()
        transcript=bytearray()
        def expect(marker,seconds=10):
            deadline=time.monotonic()+seconds
            while marker not in received:
                assert time.monotonic()<deadline,transcript.decode(errors='replace')
                if select.select([child.stdout],[],[],.1)[0]:
                    data=os.read(child.stdout.fileno(),8192)
                    assert data,transcript.decode(errors='replace')
                    received.extend(data);transcript.extend(data)
            del received[:received.index(marker)+len(marker)]
        def send(data):
            child.stdin.write(data);child.stdin.flush()
        def command(text,marker):
            send(text.encode()+b'\n');expect(marker);expect(b'fuse-x# ')
        try:
            expect(b'fuse-x# ')
            competing=subprocess.run([str(executable),'--boot-check',str(image)],
                                     capture_output=True,env=environment,timeout=10)
            assert competing.returncode!=0 and b'already in use' in competing.stdout+competing.stderr,competing
            send(b'exit\n')
            expect(b'shell stopped');expect(b'fuse-x# ')
            assert child.poll() is None,'exiting a shell stopped the kernel'
            command('/mnt/host/fairness.fxe',b'busy loops: timers, signal handlers, longjmp, stop/continue and termination passed')
            send(b'/mnt/host/fairness.fxe forever\n')
            time.sleep(.3);send(b'\x03')
            expect(b'fuse-x# ')
            command('echo interrupted-busy-loop',b'\r\ninterrupted-busy-loop\r\n')
            for _ in range(3):
                for _ in range(4):
                    send(b'/mnt/host/fairness.fxe orphan\n');expect(b'fuse-x# ')
                command('sleep 2; echo reaped',b'\r\nreaped\r\n')
            assert b'fork:' not in transcript and b'errno' not in transcript,transcript.decode(errors='replace')
            command('/mnt/host/fairness.fxe forever &',b'Running /mnt/host/fairness.fxe forever')
            # Host shutdown must also terminate a CPU-bound background task.
            child.send_signal(signal.SIGTERM)
            expect(b'filesystem synced; session stopped')
            assert child.wait(timeout=10)==0,transcript.decode(errors='replace')
            check_root(image)
        finally:
            if child.poll() is None:
                child.kill();child.communicate()
        # A headless kernel stays alive on input EOF and stops cleanly on TERM.
        headless=subprocess.Popen([str(executable),'--run',str(image),'--headless'],
                                  stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment)
        try:
            time.sleep(.5)
            assert headless.poll() is None,headless.stdout.read()
            headless.terminate()
            output=headless.communicate(timeout=10)[0]
            assert headless.returncode==0 and b'session stopped' in output,output
            assert b'fuse-x# ' not in output,output
            check_root(image)
        finally:
            if headless.poll() is None:
                headless.kill();headless.communicate()
    print('Fuse-X: shell respawn, orphan reaping, compute-loop fairness, signals, headless EOF and clean host shutdown passed')


if __name__=='__main__': main()
