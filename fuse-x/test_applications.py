#!/usr/bin/env python3
"""Smoke-test application behavior in the hosted kernel, beyond compilation."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
from test_support import record_runtime

HERE = Path(__file__).resolve().parent
CASES = [
    ('basename','basename /some/path/file','file',0),
    ('dirname','dirname /some/path/file','/some/path',0),
    ('echo','/bin/echo echo-ok','echo-ok',0),
    ('true','/bin/true','',0), ('false','/bin/false','',1),
    ('seq','seq 2 4','2\r\n3\r\n4',0),
    ('expr','expr 6 + 7','13',0),
    ('pwd','pwd','/',0), ('id','id','uid=0',0), ('whoami','whoami','root',0),
    ('groups','groups','root',0), ('logname','logname','root',0),
    ('uname','uname','FUZIX',0),
    ('mkdir','mkdir /tmp/native-dir','',0),
    ('touch','touch /tmp/native-dir/empty','',0),
    ('test','test -f /tmp/native-dir/empty','',0),
    ('cp','cp /etc/passwd /tmp/passwd','',0),
    ('cmp','cmp /etc/passwd /tmp/passwd','',0),
    ('ln','ln /tmp/passwd /tmp/linked','',0),
    ('mv','mv /tmp/linked /tmp/moved','',0),
    ('chmod','chmod 600 /tmp/moved','',0),
    ('chown','chown 0 /tmp/moved','',0),
    ('chgrp','chgrp 0 /tmp/moved','',0),
    ('cat','cat /tmp/moved','root:x:0:0:root:/:/bin/sh',0),
    ('ls','ls /tmp/native-dir','empty',0),
    ('grep','grep root /etc/passwd','root:x:',0),
    ('fgrep','fgrep root /etc/passwd','root:x:',0),
    ('head','head -1 /etc/passwd','root:x:',0),
    ('tail','tail -1 /etc/passwd','root:x:',0),
    ('sort',"echo beta > /tmp/text; echo alpha >> /tmp/text; sort /tmp/text",'alpha\r\nbeta',0),
    ('uniq','uniq /tmp/text','beta\r\nalpha',0),
    ('wc','wc -l /tmp/text','2',0),
    ('cut',"cut -d : -f 1 /etc/passwd",'root',0),
    ('tr',"echo native | tr '[a-z]' '[A-Z]'",'NATIVE',0),
    ('rev',"echo native | rev",'evitan',0),
    ('tee',"echo tee-ok | tee /tmp/tee",'tee-ok',0),
    ('sed',"echo before | sed s/before/after/",'after',0),
    ('awk',"echo '3 4' | awk '{print $1 + $2}'",'7',0),
    ('m4',"echo \"define(\\`word',\\`m4-ok')word\" | m4",'m4-ok',0),
    ('dc',"echo '7 8 + p' | dc",'15',0),
    ('picol',"picol -c 'puts tcl-ok'",'tcl-ok',0),
    ('bf',"echo '++++++++[>++++++++<-]>+.' > /tmp/program.bf; bf /tmp/program.bf; status=$?; echo; test $status = 0",'A\r\n',0),
    ('ps','ps','sh',0), ('free','free','',0), ('uptime','uptime','',0),
    ('env','env','PATH=/bin',0), ('printenv','printenv TERM','vt100',0),
    ('which','which cat','/bin/cat',0),
    ('stty','stty -a','',0),
    ('dd','dd if=/dev/zero of=/tmp/zero bs=8 count=1','',0),
    ('rm','rm /tmp/native-dir/empty','',0),
    ('rmdir','rmdir /tmp/native-dir','',0),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='fuse-x-apps-') as temporary:
        image = Path(temporary) / 'root.img'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image), '--apps', str(executable.parent / 'apps')], check=True)
        child = subprocess.Popen([str(executable), '--run', str(image)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='180'))
        received = bytearray()
        transcript = bytearray()
        checked = []

        def expect(marker):
            deadline = time.monotonic() + 20
            while marker not in received:
                assert time.monotonic() < deadline, transcript.decode(errors='replace')
                if select.select([child.stdout], [], [], .1)[0]:
                    chunk = os.read(child.stdout.fileno(), 8192)
                    assert chunk, transcript.decode(errors='replace')
                    received.extend(chunk); transcript.extend(chunk)
            end = received.index(marker) + len(marker)
            output = bytes(received[:end]); del received[:end]
            return output

        try:
            expect(b'fuse-x# ')
            for name, command, marker, status in CASES:
                child.stdin.write((command + '; echo __STATUS__:$?\n').encode()); child.stdin.flush()
                output = expect(f'\r\n__STATUS__:{status}\r\n'.encode())
                assert marker.encode() in output, (name, output.decode(errors='replace'))
                expect(b'fuse-x# ')
                checked.append(name)
                print(f'{name}: smoke tested', flush=True)
            child.stdin.write(b'shutdown\n'); child.stdin.flush()
            expect(b'session stopped')
            assert child.wait(timeout=10) == 0
        finally:
            if child.poll() is None: child.kill(); child.communicate()
        record_runtime(executable.parent, checked, 'smoke tested')
    print(f'Fuse-X: {len(checked)} application behavior checks passed')


if __name__ == '__main__':
    main()
