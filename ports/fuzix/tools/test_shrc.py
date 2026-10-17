#!/usr/bin/env python3
"""Interactive .shrc startup and persistence on private JIT/native disks."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
NATIVE=ROOT/'fuse-x'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
p.add_argument('--native',action='store_true')
p.add_argument('--engine',choices=('jit','interpreter'),default='jit')
p.add_argument('--native-build',type=Path,default=NATIVE/'build')
a=p.parse_args();build=a.build.resolve();native_build=a.native_build.resolve()
PROMPT=r'\[\e[1;32m\]\u@\h\[\e[0m\] \[\e[1;36m\]\w\[\e[0m\] \[\e[1;33m\]\$\[\e[0m\] '
CONFIG="PS1='"+PROMPT+"'\nexport PS1\nRC_SEEN=yes\necho loaded >> /tmp/shrc-count\n"
COLOR='\x1b[1;32mroot@ripple\x1b[0m \x1b[1;36m~\x1b[0m \x1b[1;33m#\x1b[0m '
SETUP="""cat > /.shrc <<'SHRC_END'
"""+CONFIG+"""SHRC_END
cat > /.profile <<'PROFILE_END'
echo UNEXPECTED_PROFILE
PROFILE_END
shutdown
"""
COMMANDS="""echo ROOT_RC:$RC_SEEN
sh -c 'echo COMMAND_RC:${RC_SEEN-unset}'
cat > /tmp/check-script <<'SCRIPT_END'
echo SCRIPT_RC:${RC_SEEN-unset}
SCRIPT_END
sh /tmp/check-script
cat /tmp/check-script | sh
sh -ic 'echo FORCED_RC:$RC_SEEN'
sh
echo CHILD_RC:$RC_SEEN
(echo FORK_RC:$RC_SEEN)
exit
echo RC_COUNT:`wc -l < /tmp/shrc-count`
mkdir '/tmp/rc home'
cat > '/tmp/rc home/.shrc' <<'HOME_END'
HOME_RC=literal_home_ok
HOME_END
cat > /tmp/.shrc <<'WRONG_END'
echo WRONG_CWD_RC
WRONG_END
cd /tmp
sh -ic 'echo CWD_RC:$RC_SEEN'
HOME='/tmp/rc home' sh -ic 'echo HOME_RC:$HOME_RC'
HOME='' sh -ic 'echo EMPTY_HOME:${RC_SEEN-unset}'
HOME=/missing-home sh -ic 'echo MISSING_HOME:${RC_SEEN-unset}'
mkdir /tmp/rc-dir /tmp/rc-fifo /tmp/rc-bad
mkdir /tmp/rc-dir/.shrc
mkfifo /tmp/rc-fifo/.shrc
HOME=/tmp/rc-dir sh -ic 'echo DIRECTORY_OK'
HOME=/tmp/rc-fifo sh -ic 'echo FIFO_OK'
cat > /tmp/rc-bad/.shrc <<'BAD_END'
if
BAD_END
HOME=/tmp/rc-bad sh
echo BAD_RC_RECOVERED
exit
cd /
echo ROOT_ALIVE:$RC_SEEN
shutdown
"""

with tempfile.TemporaryDirectory(prefix='fuzix-shrc-') as temp:
    temp=Path(temp);image=temp/'test.img'
    if a.native:
        subprocess.run(['node',str(NATIVE/'image.mjs'),'create',str(image),'--apps',str(native_build/'apps')],check=True)
    else:
        subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image)],check=True)
    count=0
    def run(commands):
        global count
        count+=1
        if a.native:
            command=[str(native_build/'fuse-x'),'--run',str(image)]
            kwargs={'input':commands,'env':dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='120')}
        else:
            command=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000',
                     '--headless','--net','off','--input',commands,'--timeout','120',*(['--jit'] if a.engine=='jit' else [])]
            kwargs={}
        result=subprocess.run(command,capture_output=True,text=True,timeout=130,**kwargs)
        output=(result.stdout+result.stderr).replace('\r','')
        log=Path('/tmp/fuzix-shrc-'+('native' if a.native else 'jit')+f'-{count}.log');log.write_text(output)
        assert result.returncode==0 and ('filesystem synced' if a.native else 'disk synced; power off.') in output,output[-10000:]
        assert 'panic:' not in output and '\nUNEXPECTED_PROFILE\n' not in output,output[-10000:]
        return output
    run(SETUP)
    output=run(COMMANDS)
    assert COLOR in output,'saved color prompt was not loaded on reboot: '+output[-7000:]
    for marker in ['ROOT_RC:yes','COMMAND_RC:unset','SCRIPT_RC:unset','FORCED_RC:yes','CHILD_RC:yes','FORK_RC:yes',
                   'literal_home_ok','CWD_RC:yes','EMPTY_HOME:unset','MISSING_HOME:unset','DIRECTORY_OK','FIFO_OK','BAD_RC_RECOVERED','ROOT_ALIVE:yes']:
        assert marker in output,(marker,output[-10000:])
    assert output.count('SCRIPT_RC:unset')>=2,output
    assert re.search(r'RC_COUNT:\s*3\s*\n',output),output
    assert '\nWRONG_CWD_RC\n' not in output,output
    assert 'syntax error' in output,output
    output=run('echo RESTART_RC:$RC_SEEN\nshutdown\n')
    assert COLOR in output and 'RESTART_RC:yes' in output,output
    if not a.native:
        output=run("cp /.shrc /tmp/keep-shrc\ncat > /.shrc <<'GTERM_END'\necho GTERM_SHRC_OK > /tmp/gterm-rc\nexit\nGTERM_END\ngterm\necho GTERM_STATUS:$?\ncat /tmp/gterm-rc\ncp /tmp/keep-shrc /.shrc\nshutdown\n")
        assert 'GTERM_STATUS:0' in output and '\nGTERM_SHRC_OK\n' in output,output
        subprocess.run([sys.executable,str(PORT/'tools/fsck.py'),str(image)],check=True)
    else:
        sys.path.insert(0,str(NATIVE));from test_support import check_root
        check_root(image)
print(('Native' if a.native else a.engine.upper())+' .shrc: automatic color prompt, restart, once-only loading, scripts skipped, HOME paths and syntax recovery passed')
