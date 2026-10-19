#!/usr/bin/env python3
"""Real root login/HOME/tilde checks on disposable Fuse-X and RVM disks."""
import argparse
import os
from pathlib import Path
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def exercise(target):
    native=target=='native'
    build=ROOT/('fuse-x/build' if native else 'build/fuzix')
    apps={name:build/('apps/'+name+'.fxe' if native else name+'.rxe') for name in ('pwd','mkdir','ls')}
    for custom in (False,True):
        home='/home/root-user' if custom else '/root'
        with tempfile.TemporaryDirectory(prefix='root-home-'+target+'-') as temporary:
            guest=Guest(temporary,target,apps,files=[
                dict(name='/etc/passwd',mode=0o100644,text='root:x:0:0:Root:'+home+':/bin/sh\n'),
                dict(name=home+'/.shrc',mode=0o100600,text='HOME_RC=loaded\n'),
            ])
            try:
                out=guest.shell('echo LOGIN:$HOME:$USER:$LOGNAME:$HOME_RC; pwd')
                assert '\nLOGIN:'+home+':root:root:loaded\n' in out,out
                assert '\n'+home+'\n' in out,out
                out=guest.shell('cd /tmp; cd ~; pwd; cd /tmp; cd; pwd')
                assert out.splitlines().count(home)==2,out
                out=guest.shell('echo ~/note; echo "~"; echo \'~\'; echo \\~')
                assert '\n'+home+'/note\n' in out,out
                assert out.splitlines().count('~')==3,out
                out=guest.shell('echo SAVED_AT_HOME > ~/note; cd /tmp; cat ~/note')
                assert '\nSAVED_AT_HOME\n' in out,out
                out=guest.shell("literal='~/note'; echo $literal")
                assert '\n~/note\n' in out,out
                out=guest.shell("PS1='child> ' sh -ic 'echo CHILD_CWD:`pwd`; echo CHILD_HOME:$HOME:$HOME_RC'")
                assert 'CHILD_CWD:/tmp\n' in out and '\nCHILD_HOME:'+home+':loaded\n' in out,out
                guest.shell("mkdir '/tmp/home space [x]'")
                out=guest.shell("HOME='/tmp/home space [x]' sh -c 'set ~; echo ARGC:$#; echo $1; cd ~; pwd'")
                assert '\nARGC:1\n' in out and '\n/tmp/home space [x]\n' in out,out
                out=guest.shell("HOME='' sh -c 'echo ~'")
                assert '\n~\n' in out,out
                guest.shell('echo COMPLETION_OK > ~/completion-note')
                os.write(guest.master,b'cat ~/comple\t\r')
                out=guest.read(guest.prompt)
                assert '\nCOMPLETION_OK\n' in out,out
                guest.shell('mkdir ~/completion-directory')
                os.write(guest.master,b'cd ~/completion-d\t\r')
                guest.read(guest.prompt)
                out=guest.shell('pwd')
                assert home+'/completion-directory\n' in out,out
                # A child shell inherits cwd while the login shell starts home.
                out=guest.shell("PS1='child> ' sh -ic 'echo CHILD_CWD:`pwd`'")
                assert 'CHILD_CWD:'+home+'/completion-directory\n' in out,out
                guest.close();guest=None
                guest=Guest(temporary,target,apps,reuse=True)
                out=guest.shell('pwd; cat ~/note; echo REBOOT:$HOME_RC')
                assert '\n'+home+'\n' in out and '\nSAVED_AT_HOME\n' in out and '\nREBOOT:loaded\n' in out,out
            finally:
                if guest:
                    Path('/tmp/root-home-'+target+('-custom' if custom else '')+'.log').write_bytes(guest.log)
                    guest.close()
    print(target+': login cwd/HOME, custom passwd home, literal tilde quoting, redirection, spaces, child cwd, completion and reboot passed',flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=('native','ripple','both'),default='both')
    args=p.parse_args()
    for target in (('native','ripple') if args.target=='both' else (args.target,)):
        exercise(target)
