#!/usr/bin/env python3
"""Exercise expr, One True Awk, make, m4 and dc through real guest processes."""
import argparse
import errno
import json
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a = p.parse_args()
build = a.build.resolve()

SCRIPT = r'''expr 12 + 3 '*' 4; echo EXPR_ARITH:$?
expr -2147483648 + 0
expr 0; echo EXPR_FALSE:$?
expr 1 / 0; echo EXPR_DIVZERO:$?
expr 2147483647 + 1; echo EXPR_OVERFLOW:$?
expr - -2147483648; echo EXPR_NEG_OVERFLOW:$?
expr 999999999999 '>' 1; echo EXPR_COMPARE_OVERFLOW:$?
expr abc : '\(a\1\)'; echo EXPR_BAD_BACKREF:$?
expr hello : 'h.*'
expr abc123 : '[a-z]*\([0-9]*\)'
expr abc : '^a.*'
expr abc : '['; echo EXPR_BAD_REGEX:$?
expr abc : '\(abc'; echo EXPR_UNCLOSED:$?
expr '(' 2 + 3 ')' '*' 4
expr len Ripple
awk -f /report.awk /records
awk -f /functions.awk
awk -f /numbers.awk
awk -f /channels.awk
cat /awk-written /awk-pipe-out
awk -f /exit.awk; echo AWK_EXIT:$?
awk '{ print $2 }' /words
awk 'BEGIN {'; echo AWK_SYNTAX:$?
awk 'BEGIN {printf("%a", 1)}'; echo AWK_HEXFMT:$?
awk 'BEGIN {printf("%100000d", 1)}'; echo AWK_WIDEFMT:$?
awk 'BEGIN {printf("%*d", 100000, 1)}'; echo AWK_STARFMT:$?
awk '{print}' /native; echo AWK_NATIVE:$?
awk '{print length($0)}' /long-record; echo AWK_LONG:$?
awk 'BEGIN { while (1) {} }' < /dev/null &
pid=$!
kill $pid
wait $pid
m4 /macros.m4
m4 /missing-m4; echo M4_MISSING:$?
m4 /bad-m4; echo M4_BAD:$?
m4 /m4-zero; echo M4_ZERO:$?
mkdir /project
cd /project
cp /project-source source
cp /project-makefile Makefile
make WORD=Ripple
cat output
make -q; echo MAKE_CURRENT:$?
make
cat builds
rm output
make -q; echo MAKE_STALE:$?
make -n
 test -f output; echo MAKE_DRY_FILE:$?
make -s WORD=again
cat output
make -r -f /implicit-makefile sample.out
cat sample.out
make -r -f /bad-makefile; echo MAKE_FAIL:$?
make -r -i -f /bad-makefile; echo MAKE_IGNORE:$?
make -r -f /cycle-makefile; echo MAKE_CYCLE:$?
make -f /missing-makefile; echo MAKE_MISSING:$?
cd /
dc /numbers.dc < /dev/null
dc /macro.dc < /dev/null
echo '6 7 * p' | dc
dc < /dev/null; echo DC_EOF:$?
libccheck
shutdown
'''
FIXTURES = {
    '/devtools-suite': SCRIPT,
    '/records': 'alice:10.5:blue\nbob:20:red\ncarol:2.25:blue\n',
    '/words': 'one two\nthree four\n',
    '/report.awk': r'''BEGIN { FS=":"; print "AWK_BEGIN" }
{ total += $2; colours[$3]++; printf("ROW:%d:%s:%.2f\n", NR, $1, $2) }
END { printf("TOTAL:%.2f MEAN:%.3f NR:%d BLUE:%d\n",total,total/NR,NR,colours["blue"]) }
''',
    '/functions.awk': r'''function fact(n) { if (n<2) return 1; return n*fact(n-1) }
BEGIN {
 print "FACT:" fact(7)
 text="abc123def45"; n=gsub(/[0-9]+/,"#",text); print "GSUB:" n ":" text
 match("xxfoo42",/foo[0-9]+/); print "MATCH:" RSTART ":" RLENGTH
 n=split("one,two,three",a,","); print "SPLIT:" n ":" a[2]
 for(i=0;i<5;i++) { if(i==1) continue; if(i==4) break; sum+=i }
 print "LOOP:" sum
 print "CASE:" toupper("Ripple") ":" tolower("UNIX")
}
''',
    '/numbers.awk': r'''BEGIN {
 printf("DECIMAL:%.3f EXPONENT:%.1f\n", .5 + .125, 1.25e2)
 printf("MATH:%.3f:%.3f:%.3f:%.3f\n",sqrt(81),log(exp(1)),sin(1),atan2(1,1))
 printf("POW:%.3f INT:%d MOD:%.1f\n",2^0.5,int(-2.75),7.5%2)
 printf("FORMATS:%08.2f:%-5s:%x:%.2e\n",1.5,"ok",255,1200)
 srand(7); x=rand(); srand(7); print "RANDOM:" (x==rand()) ":" (x>=0 && x<1)
 print "BIG:" 123456789
 printf("ROUND:%.0f:%.0f:%.2f:%.2f:%.2f\n",2.5,3.5,.125,.375,9.999)
 printf("PRECISE:%.17g:%.30f\n",.1,.125)
 printf("SMALL:%.4g:%.0e ZERO:%.2f:%.2e\n",.0000125,.0000125,0,0)
 printf("ALTERNATE:%#.0f:%#.4g SIGN:%+.2f\n",1,2,-1.25)
 print "HUGE_EXP:" (("1e65540"+0)>1e300) ":" (("1e-65540"+0)==0)
}
''',
    '/channels.awk': r'''BEGIN {
 print "AWK_FILE" > "/awk-written"; close("/awk-written")
 getline text < "/awk-written"; close("/awk-written"); print "GETLINE:" text
 command="echo from_pipe"; command | getline line; close(command); print "PIPE:" line
 command="cat > /awk-pipe-out"; print "AWK_PIPE" | command; close(command)
 print "SYSTEM:" system("true")
}
''',
    '/exit.awk': 'function stop() {exit 7}\nBEGIN {stop()}\nEND {print "AWK_END_AFTER_EXIT"}\n',
    '/native': [0x1234, 10],
    '/long-record': 'x' * 17000 + '\n',
    '/macros.m4': r'''define(`greet',`Hello, $1!')dnl
greet(`Ripple')
ifdef(`greet',`M4_DEFINED',`bad')
ifelse(`x',`x',`M4_SAME',`bad')
eval((2+3)*4)
changequote([,])dnl
define([word],[M4_EXPANDED])dnl
word
include([/macro-extra])dnl
divert(1)dnl
M4_DIVERTED
divert(0)dnl
M4_DIRECT
undivert(1)dnl
translit([abcabc],[ab],[XY])
substr([abcdef],2,3)
syscmd([echo M4_SYSCMD])dnl
''',
    '/macro-extra': 'M4_INCLUDED\n',
    '/bad-m4': "define(`unfinished'\n",
    '/m4-zero': "eval(1/0)\n",
    '/project-source': 'project source\n',
    '/project-include': 'FROM_INCLUDE=INCLUDED_OK\n',
    '/project-makefile': 'include /project-include\nWORD=default\nall: output\noutput: source\n\t@echo $(WORD) > $@\n\t@cat $< >> $@\n\t@echo $(FROM_INCLUDE) >> $@\n\t@echo built >> builds\n',
    '/implicit-makefile': '.SUFFIXES: .out .src\nsample.out: sample.src\n.src.out:\n\t@cp $< $@\nsample.src:\n\t@echo IMPLICIT_OK > $@\n',
    '/live-makefile': 'all:\n\t@echo MAKE_READY\n\t@sleep 1000\n',
    '/bad-makefile': 'broken:\n\t@false\n',
    '/cycle-makefile': 'a: b\nb: a\n',
    '/numbers.dc': '12345678901234567890 98765432109876543210 + p\nc 123456789 987654321 * p\nc 8k 1 3 / p\nc 81 v p\nc 16o 255 p\nq\n',
    '/macro.dc': '[6 7 * p]sa lax\n123 5 :b 5 ;b p\nq\n',
}


def seed(image):
    files = [{'name': name, 'mode': 0o100644,
              'data': [ord(c) for c in value] if isinstance(value, str) else value}
             for name, value in FIXTURES.items()]
    manifest = image.with_suffix('.json'); manifest.write_text(json.dumps(files))
    subprocess.run(['node', '--input-type=module', '-e', '''
import fs from 'node:fs';import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const f=process.argv[1],d=openImage(f),r=fatFile(d,'ROOTFS.IMG');
atomicWrite(f,r.replace(installRoot(r.data,JSON.parse(fs.readFileSync(process.argv[2],'utf8')))));
''', str(image), str(manifest)], cwd=ROOT, check=True)


def batch(image, jit, label):
    command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
               '--memory', '16384000', '--headless', '--input', 'sh /devtools-suite\n', '--timeout', '900']
    if jit: command.append('--jit')
    log = Path('/tmp/fuzix-devtools-' + label + '.log')
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=910)
    output = log.read_text(errors='replace').replace('\r', '')
    assert result.returncode == 0 and 'disk synced; power off.' in output, output[-10000:]
    for expected in ('24\nEXPR_ARITH:0', '-2147483648\n', 'EXPR_FALSE:1', 'EXPR_DIVZERO:2',
                     'EXPR_OVERFLOW:2', 'EXPR_NEG_OVERFLOW:2', 'EXPR_COMPARE_OVERFLOW:2', 'EXPR_BAD_BACKREF:2', '123\n', 'EXPR_BAD_REGEX:2', 'EXPR_UNCLOSED:2',
                     'AWK_BEGIN', 'ROW:1:alice:10.50', 'ROW:2:bob:20.00',
                     'TOTAL:32.75 MEAN:10.917 NR:3 BLUE:2', 'FACT:5040', 'GSUB:2:abc#def#',
                     'MATCH:3:5', 'SPLIT:3:two', 'LOOP:5', 'CASE:RIPPLE:unix',
                     'DECIMAL:0.625 EXPONENT:125.0', 'MATH:9.000:1.000:0.841:0.785',
                     'POW:1.414 INT:-2 MOD:1.5', 'FORMATS:00001.50:ok   :ff:1.20e+03',
                     'RANDOM:1:1', 'BIG:123456789',
                     'ROUND:2:4:0.12:0.38:10.00', 'PRECISE:0.10000000000000001:0.125000000000000000000000000000',
                     'SMALL:1.25e-05:1e-05 ZERO:0.00:0.00e+00',
                     'ALTERNATE:1.:2.000 SIGN:-1.25', 'HUGE_EXP:0:1', 'GETLINE:AWK_FILE', 'PIPE:from_pipe',
                     'SYSTEM:0', 'AWK_FILE', 'AWK_PIPE', 'AWK_END_AFTER_EXIT', 'AWK_EXIT:7',
                     'AWK_SYNTAX:2', 'AWK_HEXFMT:2', 'AWK_WIDEFMT:2', 'AWK_STARFMT:2', 'AWK_NATIVE:2', 'AWK_LONG:2',
                     'Hello, Ripple!', 'M4_DEFINED', 'M4_SAME', 'M4_EXPANDED', 'M4_INCLUDED',
                     'M4_DIRECT', 'M4_DIVERTED', 'XYcXYc', 'cde', 'M4_SYSCMD',
                     'M4_MISSING:1', 'M4_BAD:1', 'M4_ZERO:1', 'Ripple\nproject source',
                     'INCLUDED_OK', 'MAKE_CURRENT:0', 'MAKE_STALE:2', 'MAKE_DRY_FILE:1', 'again\nproject source',
                     'IMPLICIT_OK', 'MAKE_FAIL:1', 'MAKE_IGNORE:0', 'MAKE_CYCLE:1', 'MAKE_MISSING:1',
                     '111111111011111111100', '121932631112635269', '.33333333', 'FF',
                     '42\n', 'DC_EOF:0', 'libccheck passed'):
        assert expected in output, (expected, output[-16000:])
    assert 'panic:' not in output and 'corrupt superblock' not in output, output
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


def reboot(image, jit):
    before = image.read_bytes()
    subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image), '--install'], check=True)
    assert image.read_bytes() == before, 'no-op install changed a clean disk'
    cmd = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
           '--memory', '16384000', '--headless', '--input',
           'cat /awk-written /awk-pipe-out /project/output\nshutdown\n', '--timeout', '120']
    if jit: cmd.append('--jit')
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=130)
    output = result.stdout.replace('\r', '')
    assert result.returncode == 0 and 'disk synced; power off.' in output, output
    assert 'AWK_FILE\nAWK_PIPE\nagain\nproject source\nINCLUDED_OK' in output, output


def live(image, jit):
    master, slave = pty.openpty()
    cmd = [str(ROOT / 'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--timeout', '600']
    if not jit: cmd.append('--no-jit')
    proc = subprocess.Popen(cmd, cwd=ROOT, env=dict(os.environ, FUZIX_BUILD=str(build)),
                            stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave); buffer = b''; transcript = bytearray()
    def until(token, timeout=120):
        nonlocal buffer
        deadline = time.monotonic() + timeout
        while token not in buffer:
            if time.monotonic() > deadline: raise AssertionError((token, buffer[-4000:]))
            if select.select([master], [], [], 0.2)[0]:
                try: data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO: raise AssertionError(buffer[-4000:]) from error
                    raise
                assert data, buffer[-4000:]
                buffer += data; transcript.extend(data)
        end = buffer.index(token) + len(token); out, buffer = buffer[:end], buffer[end:]
        return out.replace(b'\r', b'')
    def send(data): os.write(master, data)
    def status():
        send(b'echo INTERRUPT:$?\r')
        assert b'INTERRUPT:130' in until(b'riplix# ')
    try:
        until(b'riplix# ')
        send(b"awk 'BEGIN {print \"AWK_READY\"; fflush(); while(1) {}}'\r")
        until(b'\nAWK_READY\r'); send(b'\x03'); until(b'riplix# '); status()
        send(b'm4\r'); send(b"syscmd(`echo M4_READY')dnl\r")
        until(b'\nM4_READY\r'); send(b'\x03'); until(b'riplix# '); status()
        send(b'dc\r'); send(b'42p\r')
        until(b'\n42\r'); send(b'\x03'); until(b'riplix# '); status()
        send(b'make -f /live-makefile\r')
        until(b'\nMAKE_READY\r'); send(b'\x03'); until(b'riplix# '); status()
        send(b'echo SHELL_ALIVE\r'); assert b'\nSHELL_ALIVE\n' in until(b'riplix# ')
        send(b'shutdown\r'); until(b'disk synced; power off.'); assert proc.wait(timeout=10) == 0
    finally:
        Path('/tmp/fuzix-devtools-live-' + ('jit' if jit else 'interpreter') + '.log').write_bytes(transcript)
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master)


with tempfile.TemporaryDirectory(prefix='fuzix-devtools-') as directory:
    for jit in ([True, False] if a.engine == 'both' else [a.engine == 'jit']):
        label = 'jit' if jit else 'interpreter'; image = Path(directory) / (label + '.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
        seed(image); batch(image, jit, label)
        reboot(image, jit); live(image, jit)
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(label + ': expr, awk, make, m4, dc, libc, interrupts, persistence and upgrade checks passed', flush=True)
