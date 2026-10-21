#!/usr/bin/env python3
"""Exercise the REPL command interface on host or disposable real guests."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]


def fixtures(base):
    return {
        base + '/counter.syma': '''{Universe {Program {Count 0}} {Rules
 {R "Counter/Inc" {Apply Counter/Inc {Program {Count n_} tail..}}
                    {Program {Count {Add n_ 1}} tail..}}}}''',
        base + '/tiny.syma': '{Module Tiny {Export Twice} {Rules {R twice {Twice x_} {Mul x_ 2}}}}',
        base + '/input-demo.syma': (PORT/'examples/input-demo.syma').read_text(),
        base + '/bundled.syma': '{Module Demo {Export Double} {Program {Double 21} {Args}} {Rules {R d {Double x_} {Mul x_ 2}}}}',
    }


def scenarios(base):
    return [
        ('inspect-edit', ''':h
:u
:p
:rs
:rr
:macro-scopes
:rule double Double(x_) -> Mul(x_ 2)
:rule double
:rules
Double(21)
:edit double Double(x_) -> Mul(x_ 3)
Double(4)
:drop double
:undo
Double(5)
:trace verbose Double(6)
:trace diff Double(7)
:trace stats Double(8)
:set trace on
Double(9)
:set trace off
:why Double(3)
:rule guarded Positive(x_) -> x_ :guard Gt(x_ 0)
:why Positive(-1)
:apply double Double(10)
:exec double 11
:history 5
:rule
{R "full" {Full x_} x_}
.
Full(314)
:Add 1
:quit
'''.replace(':Add 1\n', ''), ['Syma REPL', 'Universe', 'Program', 'Rules (1)', '\n42\n', '\n12\n', '\n15\n',
                                   'Trace statistics', 'pattern:', 'before:', 'guard is not True', '{Mul 10 3}', '\n33\n', '\n314\n']),
        ('match', ''':match {Pair x_ xs..} :rewrite {Out xs.. x_} :target {Pair 1 2 3}
:m n_ :norm Add(3 4)
:match {Program p..}
:match
{X x_}
:rewrite
{Y x_}
:target
{X "literal :target :rewrite"}
:end
:match {Program p..} :rewrite {Program {Changed p..}} :universe
:p
:undo
:p
:quit
''', ['Match succeeded', 'Bindings', 'Rest', '{Out 2 3 1}', '7', 'literal :target :rewrite', 'Changed']),
        ('state-files', f''':bundle "{base}/counter.syma"
:apply Inc
:p
:undo
:p
:save "{base}/saved.syma"
:save "{base}/saved.json"
:clear
:load "{base}/saved.syma"
:reload
:clear
:load "{base}/saved.json"
:bundle "{base}/bundled.syma" :args --input "hello world" --flag --count=2 -o output.txt positional
:p
:norm show
:p
:reload
:export Demo "{base}/exported.syma"
:quit
''', ['{Count 1}', '{Count 0}', 'Universe saved', 'Universe loaded', 'Universe bundled', 'Universe normalized', '42', 'hello world', '{KV \"flag\" Empty}', '{KV \"o\" \"output.txt\"}', '{KV 0 \"positional\"}', 'Compiled library exported']),
        ('imports', f''':import "{base}/tiny.syma" as T open
T/Twice(6)
Twice(7)
:save "{base}/imports.json"
:clear
:load "{base}/imports.json"
T/Twice(8)
:import Core/List as L open
L/Len({{List 1 2 3}})
L/Head({{List 777 888}})
Len({{List 1 2 3 4}})
:rule size LengthOf(x_) -> L/Len(x_)
LengthOf({{List 99 100}})
:undo
:undo
T/Twice(10)
:quit
''', ['Module imported', '\n12\n', '\n14\n', '\n16\n', '\n3\n', '\n777\n', '\n4\n', '\n2\n', '\n20\n']),
        ('input-effects', f':bundle \"{base}/input-demo.syma\"\n:norm\nAlice\nx\n:quit\n', ['Hello Alice!', 'You pressed: x', 'Universe normalized']),
        ('multiline', '''Add(1 \\
 2)
.
:rule multiline
identity {Identity x_}
 -> x_
:end
Identity(345)
r"raw\\"
:quit
''', ['\n3\n', '\n345\n', 'raw']),
    ]


def check(text, markers):
    text = '\n' + text.replace('\r', '') + '\n'
    for marker in markers:
        if marker not in text:
            raise AssertionError('Missing ' + repr(marker) + '\n' + text[-8000:])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('target', choices=['host','native','ripple'])
    p.add_argument('--binary', type=Path)
    a = p.parse_args()
    folder, name = {'host':('host','syma'),'native':('fuse-x','syma.fxe'),'ripple':('fuzix','syma.rxe')}[a.target]
    binary = (a.binary or PORT/'build'/folder/name).resolve()
    directory = Path(tempfile.mkdtemp(prefix='repl-'+a.target+'-',dir=PORT/'build'))
    start = time.monotonic()
    if a.target == 'host':
        base = str(directory)
        for path, source in fixtures(base).items(): Path(path).write_text(source)
        env = os.environ | {'HOME':base,'SYMA_PATH':str(PORT/'library')}
        for label, source, expected in scenarios(base):
            r = subprocess.run([binary,'repl'],input=source,text=True,capture_output=True,env=env,timeout=60)
            (directory/(label+'.log')).write_text(r.stdout+r.stderr)
            assert r.returncode == 0, (label,r.returncode,r.stdout,r.stderr)
            check(r.stdout+r.stderr,expected)
        # Failed state changes must roll back; inspection/evaluation remain usable.
        source = ':rule keep Keep(x_) -> x_\n:edit keep bad\n:drop missing\n:match x_ :rewrite Bad :universe\nKeep(222)\n:quit\n'
        r = subprocess.run([binary,'repl'],input=source,text=True,capture_output=True,env=env,timeout=20)
        assert r.returncode == 1 and '\n222\n' in '\n'+r.stdout, (r.stdout,r.stderr)
        # Long lines cross fgets chunks, including whitespace at a boundary.
        source = '{StrLen "' + 'a'*1013 + ' ' + 'b'*4000 + '"}\n:quit\n'
        r = subprocess.run([binary,'repl'],input=source,text=True,capture_output=True,env=env,timeout=20)
        assert r.returncode==0 and '5014' in r.stdout, (r.stdout,r.stderr)
        # Interactive sessions retain bounded history under HOME, privately.
        import pty, select, tty
        def terminal(source):
            master, slave = pty.openpty(); tty.setraw(slave)
            process=subprocess.Popen([binary,'repl'],stdin=slave,stdout=slave,stderr=slave,env=env)
            os.close(slave); data=bytearray(); deadline=time.monotonic()+10
            try:
                while b'syma> ' not in data:
                    if time.monotonic()>deadline:raise AssertionError('REPL prompt timeout')
                    if select.select([master],[],[],.1)[0]:data.extend(os.read(master,65536))
                os.write(master,source.encode())
                while process.poll() is None:
                    if time.monotonic()>deadline:raise AssertionError('interactive REPL timeout')
                    if select.select([master],[],[],.1)[0]:
                        try:data.extend(os.read(master,65536))
                        except OSError:break
                process.wait(timeout=3)
                return data.decode(errors='replace')
            finally:
                if process.poll() is None:process.kill();process.wait()
                os.close(master)
        terminal('InteractiveHistory123\n:quit\n')
        check(terminal(':history 20\n:quit\n'),['InteractiveHistory123'])
        assert (directory/'.syma/history.syma').stat().st_mode & 0o777 == 0o600
    else:
        sys.path.insert(0,str(ROOT/'ports/tls/tests')); from guest import Guest
        base='/tmp'
        entries=[dict(name=path,text=text,mode=0o100644) for path,text in fixtures(base).items()]
        entries.append(dict(name='/root/.keep',text='',mode=0o100644))
        cases=scenarios(base)
        for label,source,_ in cases:entries.append(dict(name='/tmp/'+label+'.repl',text=source,mode=0o100644))
        for f in (PORT/'library').glob('*.syma'):
            entries.append(dict(name='/usr/share/syma/library/'+f.name,path=str(f),mode=0o100644))
        vm=Guest(directory,a.target,{'syma':binary},entries,blocks=65536,timeout=1200)
        try:
            vm.shell('HOME=/root; export HOME')
            for label,_,expected in cases:
                text=vm.shell('syma repl < /tmp/'+label+'.repl; echo REPL_STATUS:$?',600)
                check(text,['REPL_STATUS:0',*expected])
            # Exercise the real guest tty path, not only redirected scripts.
            os.write(vm.master,b'syma repl\r');vm.read(b'syma> ',30)
            os.write(vm.master,b'GuestHistory987\r');vm.read(b'syma> ',30)
            os.write(vm.master,b':quit\r');vm.read(vm.prompt,30)
            os.write(vm.master,b'syma repl\r');vm.read(b'syma> ',30)
            os.write(vm.master,b':history 20\r');vm.read(b'GuestHistory987',30)
            vm.read(b'syma> ',30)
            os.write(vm.master,b':quit\r');vm.read(vm.prompt,30)
        finally:vm.close()
    report=dict(target=a.target,status='passed',seconds=round(time.monotonic()-start,3),logs=str(directory))
    (PORT/'build'/('repl-result-'+a.target+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
