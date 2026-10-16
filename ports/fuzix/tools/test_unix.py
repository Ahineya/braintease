#!/usr/bin/env python3
"""Exercise upstream Unix binaries, shell plumbing, and persistent upgrades in RVM."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import shutil
PORT=Path(__file__).resolve().parents[1]
ROOT=PORT.parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
a=p.parse_args();build=a.build.resolve()
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']
engine_label = 'JIT and interpreter' if a.engine == 'both' else a.engine

def run(image,commands,jit):
    args=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000','--headless','--input',commands,'--timeout','180']
    if jit:args+=['--jit']
    live=Path('/tmp/fuzix-unix-live.log')
    with live.open('w') as log:
        r=subprocess.run(args,text=True,stdout=log,stderr=subprocess.STDOUT,timeout=190)
    output=live.read_text().replace('\r','')
    assert r.returncode==0 and 'Ripple: disk synced; power off.' in output,output
    output=output.split('Starting /init\n')[-1]
    output='\n'.join(line for line in output.splitlines()
                     if not line.startswith(('fuzix# ', '> ')))
    assert 'FAILED' not in output and 'panic:' not in output,output
    return output

def section(text,start,end):
    return text.split(start+'\n',1)[1].split(end+'\n',1)[0].replace('fuzix# ','')

def check_image(image):
    try:
        subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    except subprocess.CalledProcessError:
        saved=Path('/tmp/fuzix-unix-failed.img')
        shutil.copyfile(image,saved)
        print('Saved failed filesystem:',saved,flush=True)
        raise

with tempfile.TemporaryDirectory(prefix='fuzix-unix-') as d:
    image=Path(d)/'unix.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image)],check=True)
    # Device deletion followed by pipe creation must not recycle the old
    # device number as a data-block address. The final fsck checks ownership.
    commands=r"""libccheck
mkdir -p /work/nested
cd /work/nested
pwd
echo -e 'beta\012alpha\012beta' > words
sort words | uniq -c > counts
echo COUNTS
cat counts
echo ENDCOUNTS
grep -n '^alpha$' words
fgrep -n beta words
sed 's/beta/BETA/g' words > upper
head -n 2 upper
tail -n 1 upper
wc -lwc words
echo CUTBEGIN
cut -c1-2 words
echo CUTEND
tr a-z A-Z < words > caps
cp words copy
cmp words copy
mv copy moved
ln moved linked
rm moved
cmp words linked
cp -R /work /work-copy
cmp words /work-copy/nested/words
rm -r /work-copy
chmod 600 linked
ls -l linked
chown root linked
chgrp root linked
touch touched
basename /work/nested/words
dirname /work/nested/words
printenv PATH
env MESSAGE=hello printenv MESSAGE
id
whoami
uname -a
df -i /
du -s .
which sed sh
sleep 0; sleep 1; echo SLEPT:$?
kill -0 1; echo KILL:$?
date -u
echo EXPANDBEGIN
echo -e 'a\011b' | expand -t 4
echo EXPANDEND
echo COLRMBEGIN
echo abcdef | colrm 2 4
echo COLRMEND
mkfifo fifo
mknod another-fifo p
mknod null-copy c 1 0
echo discarded > null-copy
test -p fifo; echo FIFO:$?
test -p another-fifo; echo MKNOD:$?
rm fifo another-fifo null-copy
seq 1 3 | tee numbers | wc -l
seq 1 3 | xargs echo
false; echo STATUS:$?
true; echo STATUS:$?
test -f words; echo TEST:$?
test 7 -gt 9; echo TEST:$?
cat missing 2> errors; echo CAT:$?
cat errors
echo KEEP > /session-note
sh -c 'echo CHILD; exit 7'; echo SHELL:$?
echo -e 'a\012edited line\012.\012w /edited\012q' | ed
cat /edited
rev numbers
comm numbers numbers
sum words
od -c words
ps -ef
head -n 0 words; echo ZERO:$?
echo -n finalword > no-newline
wc -w no-newline
yes ripple | head -n 3
cat /dev/null
sync
shutdown
"""
    out=run(image,commands,engines[0])
    Path('/tmp/fuzix-unix-test-output.log').write_text(out)
    for wanted in ['libccheck passed','/work/nested','alpha','Files are identical','/bin','hello','uid=0(root)','root','FUZIX','Ripple','/bin/sed','/bin/sh','STATUS:1','STATUS:0','TEST:0','TEST:1','CAT:1','No such file or directory','CHILD','SHELL:7','edited line','SLEPT:0','KILL:0','FIFO:0','MKNOD:0','UTC']:
        assert wanted in out,(wanted,out)
    assert 'syntax error' not in out and '???' not in out,out
    assert ' ps\n' in out and ' sh\n' in out,out
    assert section(out,'CUTBEGIN','CUTEND') == 'be\nal\nbe\n',out
    assert section(out,'EXPANDBEGIN','EXPANDEND') == 'a   b\n',out
    assert section(out,'COLRMBEGIN','COLRMEND') == 'aef\n',out
    assert '0000020' in out,out
    counts=section(out,'COUNTS','ENDCOUNTS')
    assert '1 alpha' in counts and '2 beta' in counts,counts
    check_image(image)
    # Upgrade the managed executables in place, then verify user files after reboot.
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image),'--install'],check=True)
    out=run(image,'mknod /recycled c 1 0\nrm /recycled\necho recycled | cat\necho AFTER > /after-pipe\ncat /after-pipe\nrm /after-pipe\ncat /session-note\ncat /edited\ncat /work/nested/words\nrm -r /work\nrm /session-note /edited\nsync\nshutdown\n',engines[-1])
    assert 'KEEP\n' in out and 'edited line\n' in out and 'beta\012alpha\012beta\n' in out,out
    assert 'recycled\n' in out and 'AFTER\n' in out,out
    assert 'rm:' not in out,out
    check_image(image)
print('Unix userspace passed: upstream commands, pipes/redirection, statuses, shell, ed, upgrade, reboot persistence; ' + engine_label)
