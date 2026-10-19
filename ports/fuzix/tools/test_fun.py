#!/usr/bin/env python3
"""Run cowsay and the Picol Tcl subset in RVM, including live input and persistence."""
import argparse
import hashlib
import errno
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT/'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
options = p.parse_args()
engines = [True, False] if options.engine == 'both' else [options.engine == 'jit']
engine_label = 'JIT and interpreter' if options.engine == 'both' else options.engine
build = options.build.resolve()

SCRIPT = r'''# Core Tcl-style syntax and 32-bit arithmetic.
  # Leading whitespace must not turn this comment into a command.
puts ARITH:[expr {3 * (2 + 4)}]
puts MOD:[% 17 5]
puts FLOOR:[expr {-7 / 3}]:[expr {-7 % 3}]
puts LAZY:[expr {1 || (1 / 0)}]:[expr {0 && $missing}]:[expr {0 && -(-2147483648)}]
puts OVERFLOW:[catch {expr {2147483647 + 1}}]
puts MUL_OVERFLOW:[catch {expr {100000 * 100000}}]
puts DIV_OVERFLOW:[catch {expr {-2147483648 / -1}}]
puts BADINT:[catch {+ nope 2}]:[catch {+ { } 2}]
set {a b} braced-variable
puts VAR:${a b}
proc fact {n} {
    if {$n <= 1} {return 1}
    set m [fact [expr {$n - 1}]]
    return [expr {$n * $m}]
}
puts FACT:[fact 8]
proc total {first args} {
    set value $first
    foreach item $args {incr value $item}
    return $value
}
puts VARARGS:[total 1 2 3 4]
set result {}
for {set n 0} {$n < 6} {incr n} {
    if {$n == 1} {continue}
    if {$n == 4} {break}
    append result $n
}
puts FOR:$result
set n 0
while {< $n 3} {incr n}
puts PREFIX:$n
set words [list {} {two words} {a\b} {$literal} {[brackets]} {quote"here} {{nested}}]
puts LIST:[llength $words]:[lindex $words 1]:[lindex $words 2]:[lindex $words end]
lappend words LAST
puts APPEND:[llength $words]:[lindex $words end]
puts ESCAPES:[string length "a\nb\tc"]:[string equal [lindex $words 3] {$literal}]
puts STRING:[string index Ripple end]
for {set n 0} {$n < 70} {incr n} {
    catch {fact 1 2}
    catch {unknown_command}
    proc again {} {return ok}
}
puts REDEFINE:[again]
proc self {} {proc self {} {return changed}}
puts ACTIVE:[catch {self}]
proc recurse {} {recurse}
puts DEPTH:[catch {recurse}]
puts INCOMPLETE:[catch {puts [}]
set f [open /tcl-state w]
puts $f one
puts $f {two lines}
close $f
set f [open /tcl-state a]
puts $f three
close $f
set f [open /tcl-state r]
puts READ:[read $f 3]
puts GET0:[gets $f line]:[string length $line]
puts GET1:[gets $f line]:$line
puts GET2:[gets $f line]:$line
puts GETEOF:[gets $f line]:[eof $f]
close $f
puts CHANNELERR:[catch {close file7}]:[catch {open /no-such-file}]
source /tcl-source.tcl
puts SOURCE:$sourced
puts EXEC:[exec echo {semi; literal}]
puts EXECERR:[catch {exec false}]
puts ARGS:$argc:[lindex $argv 0]:[lindex $argv 1]
puts SCRIPT_DONE
'''
COMMANDS = r'''cowsay Hello Ripple
cowsay -d moo
cowsay -s moo
cowsay -W 8 {placeholder}
echo piped_message | cowsay -w
cowsay < /dev/null
cowsay -- -literal
cowsay -W 0 nope; echo BADWIDTH:$?
cowsay -z nope; echo BADOPTION:$?
cowsay --help
cowsay --version
cowsay < /cow-long > /cow-wrapped; echo WRAP:$?
cowsay < /cow-too-long > /cow-rejected; echo COW_LIMIT:$?
picol /tcl-suite.tcl first 'two words'
tcl -c 'puts ALIAS:[expr {6 * 7}]'
tclsh -c 'puts OTHER_ALIAS'
echo 'puts PIPE_TCL' | tcl
tcl -c 'puts {'; echo BADBRACE:$?
tcl -c 'puts [foo\'; echo BADBRACKET:$?
tcl -c 'puts "unterminated'; echo BADQUOTE:$?
tcl -c 'error deliberate'; echo SCRIPT_ERROR:$?
tcl -c 'exit 7'; echo TCL_EXIT:$?
tcl -c 'break'; echo BAD_BREAK:$?
tcl -c 'set f [open /tcl-exact r]; puts EXACT:[string length [read $f]]; close $f'
tcl -c 'set f [open /tcl-over r]; read $f'; echo READ_LIMIT:$?
tcl /tcl-big.tcl
tcl /tcl-oversize.tcl; echo SCRIPT_LIMIT:$?
tcl /hello.tcl coffee
cat /tcl-state
shutdown
'''.replace('{placeholder}', "'one two three four five six seven'")

def seed(image, files):
    manifest = image.with_suffix('.json'); manifest.write_text(json.dumps(files))
    source = r'''
import fs from 'node:fs';import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const f=process.argv[1],d=openImage(f),root=fatFile(d,'ROOTFS.IMG');
atomicWrite(f,root.replace(installRoot(root.data,JSON.parse(fs.readFileSync(process.argv[2],'utf8')))));
'''
    subprocess.run(['node','--input-type=module','-e',source,str(image),str(manifest)],cwd=ROOT,check=True)

def run(image, commands, jit, label):
    cmd = [str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
           '--memory','16384000','--headless','--net','off','--input',commands,'--timeout','360']
    if jit: cmd.append('--jit')
    log = Path('/tmp/fuzix-fun-' + label + '.log')
    with log.open('w') as stream:
        result = subprocess.run(cmd,stdout=stream,stderr=subprocess.STDOUT,timeout=370)
    text = log.read_text().replace('\r','').split('Starting /init\n')[-1]
    text = '\n'.join(line for line in text.splitlines() if not line.startswith(('fuzix# ', '> ')))
    assert result.returncode == 0 and 'disk synced; power off.' in text, text
    assert 'panic:' not in text and 'Out of memory' not in text, text
    return text

def live(image, jit):
    master, slave = pty.openpty()
    cmd = [str(ROOT/'scripts/run-fuzix.sh'),'--run-only','--disk',str(image),'--net','off','--timeout','240']
    if not jit: cmd.append('--no-jit')
    proc = subprocess.Popen(cmd,cwd=ROOT,env=dict(os.environ,FUZIX_BUILD=str(build)),
                            stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
    os.close(slave); buffer = b''; transcript = bytearray()
    def until(token, timeout=55):
        nonlocal buffer
        deadline = time.monotonic() + timeout
        while token not in buffer:
            if time.monotonic() > deadline: raise AssertionError((token,buffer[-4000:]))
            if select.select([master],[],[],0.2)[0]:
                try: data = os.read(master,65536)
                except OSError as e:
                    if e.errno == errno.EIO: raise AssertionError(buffer[-4000:]) from e
                    raise
                assert data, buffer[-4000:]
                buffer += data; transcript.extend(data)
        end = buffer.index(token)+len(token); out,buffer=buffer[:end],buffer[end:]
        return out.replace(b'\r',b'')
    def send(data): os.write(master,data)
    try:
        until(b'fuzix# '); send(b'tcl\r'); until(b'tcl> ')
        send(b'expr {2 + 3 * 4}\r'); assert b'\n14\n' in until(b'tcl> ')
        send(b'proc greet {name} {\r'); until(b'> ')
        send(b'puts "hello $name"\r'); until(b'> ')
        send(b'}\r'); until(b'tcl> ')
        send(b'greet Ripple\r'); assert b'\nhello Ripple\n' in until(b'tcl> ')
        send(b'unknown_command\r'); assert b'No such command' in until(b'tcl> ')
        send(b'exit\r'); until(b'fuzix# ')
        send(b'tcl\r'); until(b'tcl> '); send(b'\x04'); until(b'fuzix# ')
        send(b'tcl\r'); until(b'tcl> '); send(b'\x03'); until(b'fuzix# ')
        send(b'echo INTERRUPT:$?\r'); assert b'INTERRUPT:130' in until(b'fuzix# ')
        send(b"tcl -c 'puts COMPUTING; flush stdout; while {1} {}'\r")
        until(b'\nCOMPUTING\r'); send(b'\x03'); until(b'fuzix# ')
        send(b'echo COMPUTE:$?\r'); assert b'COMPUTE:130' in until(b'fuzix# ')
        send(b'shutdown\r'); until(b'disk synced; power off.'); assert proc.wait(timeout=10)==0
    finally:
        Path('/tmp/fuzix-fun-live-'+('jit' if jit else 'interpreter')+'.log').write_bytes(transcript)
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master)

with tempfile.TemporaryDirectory(prefix='fuzix-fun-') as tmp:
    for jit in engines:
        label = 'jit' if jit else 'interpreter'; image=Path(tmp)/(label+'.img')
        subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image)],check=True)
        fixtures = {
            '/tcl-suite.tcl':SCRIPT, '/tcl-source.tcl':'set sourced source_ok\n',
            '/cow-long':'longword'*80+'\nsecond line\n', '/cow-too-long':'x'*4097,
            '/tcl-exact':'x'*30000, '/tcl-over':'x'*30001,
            '/tcl-big.tcl':'# filler\n'*1100+'puts BIG_TAIL\n',
            '/tcl-oversize.tcl':'#'+'x'*30000,
        }
        seed(image,[{'name':n,'mode':0o100644,'data':list(s.encode())} for n,s in fixtures.items()])
        output=run(image,COMMANDS,jit,label)
        for expected in ['< Hello Ripple >','(XX)','(**)','(OO)','Nothing to say.',
                         'piped_message','< -literal >','BADWIDTH:2','BADOPTION:2','WRAP:0','COW_LIMIT:1',
                         'ARITH:18','MOD:2','FLOOR:-3:2','LAZY:1:0:0','OVERFLOW:1','MUL_OVERFLOW:1','DIV_OVERFLOW:1',
                         'BADINT:1:1','VAR:braced-variable','FACT:40320','VARARGS:10','FOR:023','PREFIX:3',
                         'LIST:7:two words:a' + chr(92) * 1 + 'b:{nested}','APPEND:8:LAST','ESCAPES:5:1','STRING:e',
                         'REDEFINE:ok','ACTIVE:1','DEPTH:1','INCOMPLETE:1','READ:one','GET0:0:0',
                         'GET1:9:two lines','GET2:5:three','GETEOF:-1:1','CHANNELERR:1:1','SOURCE:source_ok',
                         'EXEC:semi; literal','EXECERR:1','ARGS:2:first:two words','SCRIPT_DONE','ALIAS:42',
                         'OTHER_ALIAS','PIPE_TCL','BADBRACE:1','BADBRACKET:1','BADQUOTE:1','SCRIPT_ERROR:1',
                         'TCL_EXIT:7','BAD_BREAK:1','EXACT:30000','READ_LIMIT:1','BIG_TAIL','SCRIPT_LIMIT:1',
                         'Hello, coffee! Tcl is alive.','square(5) = 25']:
            assert expected in output,(expected,output)
        subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
        before=hashlib.file_digest(image.open('rb'), 'sha256').digest()
        subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image),'--install'],check=True)
        assert hashlib.file_digest(image.open('rb'), 'sha256').digest()==before,'a clean no-op upgrade changed the disk'
        output=run(image,'cat /tcl-state /tcl-visits\ntcl /hello.tcl again\nshutdown\n',jit,label+'-reboot')
        assert 'one\ntwo lines\nthree' in output and 'Hello, coffee!' in output and 'Hello, again!' in output,output
        live(image,jit)
        subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
        print(label+': cowsay, Tcl scripts/REPL, bounds, channels, subprocesses, interrupts, upgrade and persistence passed',flush=True)
print('Fun tools passed under ' + engine_label)
