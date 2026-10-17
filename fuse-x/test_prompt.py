#!/usr/bin/env python3
"""Exercise PS1/PS2 escapes and editing through the real native Bourne shell."""
import argparse
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time
from test_support import check_root, record_runtime

HERE=Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    executable=parser.parse_args().executable.resolve()
    environment=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='45')
    with tempfile.TemporaryDirectory(prefix='fuse-x-prompt-') as temporary:
        image=Path(temporary)/'root.img'
        subprocess.run(['node',str(HERE/'image.mjs'),'create',str(image),'--apps',str(executable.parent/'apps')],check=True)
        child=subprocess.Popen([str(executable),'--run',str(image)],stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment)
        received=bytearray();transcript=bytearray()
        def expect(token,pattern=False):
            deadline=time.monotonic()+12
            while True:
                match=re.search(token,received) if pattern else None
                end=match.end() if match else received.find(token)+len(token) if not pattern and token in received else 0
                if end:
                    output=bytes(received[:end]);del received[:end];return output
                assert time.monotonic()<deadline,transcript.decode(errors='replace')
                if select.select([child.stdout],[],[],.1)[0]:
                    data=os.read(child.stdout.fileno(),8192)
                    assert data,transcript.decode(errors='replace')
                    received.extend(data);transcript.extend(data)
        def send(data):
            child.stdin.write(data);child.stdin.flush()
        def line(text):send(text.encode()+b'\n')
        def prompt(spec,expected):
            line("PS1='"+spec+"'");expect(expected)
        def editing(prefix,wrapped=False):
            start=len(transcript)
            send(b'echo ac\x1b[Db\x1b[F')
            expect(b'\r\x1b['+str((prefix+8)%80).encode()+b'C')
            if wrapped:assert b'HEADER' not in transcript[start:],transcript[start:]
            else:assert b'\r\x1b['+str(prefix).encode()+b'C\x1b[J' in transcript[start:],transcript[start:]
            send(b'\n');expect(b'\r\nabc\r\n')
        try:
            expect(b'fuse-x# ')
            identity=r'\u@\h|\H|\s|\w|\W|\$ '
            base=b'root@ripple|ripple|sh|~|~|# '
            prompt(identity,base)
            line('mkdir /tmp/ps-dir; HOME=/tmp; cd /tmp/ps-dir')
            expect(b'root@ripple|ripple|sh|~/ps-dir|ps-dir|# ')
            color=b'\x1b[32mP>\x1b[0m '
            prompt(r'\[\e[32m\]P>\[\e[0m\] ',color)
            editing(3);expect(color)
            multiline=b'HEADER\r\n\x1b[31mM>\x1b[0m '
            prompt(r'HEADER\n\[\e[31m\]M>\[\e[0m\] ',multiline)
            start=len(transcript);editing(3)
            # Editing must leave the earlier prompt line in place.
            assert transcript[start:].count(b'HEADER')<=1
            expect(multiline)
            long_tail='p'*90+'> '
            prompt('HEADER\\n'+long_tail,('HEADER\r\n'+long_tail).encode())
            editing(92,wrapped=True);expect(('HEADER\r\n'+long_tail).encode())
            prompt(r'\u:\w> ',b'root:~/ps-dir> ')
            line('false');expect(b'root:~/ps-dir> ')
            line('echo $?');expect(b'\r\n1\r\n');expect(b'root:~/ps-dir> ')
            prompt('done> ',b'\r\ndone> ')
            line(r"PS2='\[\e[33m\]C:\W>\[\e[0m\] '");expect(b'done> ')
            secondary=b'\x1b[33mC:ps-dir>\x1b[0m '
            line('if true');expect(secondary)
            line('then echo continuation-ok');expect(secondary)
            line('fi');expect(b'\r\ncontinuation-ok\r\n');expect(b'done> ')
            line(r"PS1='TIME:\t|\T|\A|\@|\d> '")
            expect(rb'TIME:\d\d:\d\d:\d\d\|\d\d:\d\d:\d\d\|\d\d:\d\d\|\d\d:\d\d [AP]M\|[A-Z][a-z]{2} [A-Z][a-z]{2} \d\d> ',pattern=True)
            literal=b'`touch /tmp/prompt-side-effect`> '
            prompt(literal.decode(),b'\r\n'+literal)
            line('test -f /tmp/prompt-side-effect; echo PROMPT_SIDE:$?')
            expect(b'\r\nPROMPT_SIDE:1\r\n');expect(literal)
            prompt('fuse-x# ',b'\r\nfuse-x# ')
            line('shutdown');expect(b'filesystem synced; session stopped')
            assert child.wait(timeout=10)==0,transcript.decode(errors='replace')
            check_root(image)
        finally:
            if child.poll() is None:child.kill();child.communicate()
    subprocess.run([sys.executable,str(HERE.parent/'ports/fuzix/tools/test_shrc.py'),
                    '--native','--native-build',str(executable.parent)],check=True)
    record_runtime(executable.parent,['sh'],'prompt and editor integration tested')
    print('Fuse-X: dynamic PS1/PS2, identity/path/time escapes, colors, multiline/wrapped editing, status and literal prompts passed')


if __name__=='__main__':main()
