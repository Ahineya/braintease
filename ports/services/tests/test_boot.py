#!/usr/bin/env python3
"""Boot real Fuse-X with opt-in profiles on disposable disks only."""
from pathlib import Path
import json,os,subprocess,sys,tempfile,time
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/fuzix/tools'))
from test_mux_restore import Terminal
BUILD=ROOT/'fuse-x/build'
OUT=ROOT/'build/services'
OUT.mkdir(exist_ok=True)
class BootGuest(Guest):
    def __init__(self,directory,mux=False,rescue=False):
        self.first_marker=b'\x1b[?1003h' if mux else None
        old=os.environ.get('FUSE_X_TEST_KERNEL')
        if rescue:
            wrapper=Path(directory)/'rescue'
            wrapper.write_text('#!/bin/sh\nexec '+str(BUILD/'fuse-x')+' "$@" --rescue\n');wrapper.chmod(0o700)
            os.environ['FUSE_X_TEST_KERNEL']=str(wrapper)
        try:super().__init__(directory,'native',{},reuse=True)
        finally:
            if old is None:os.environ.pop('FUSE_X_TEST_KERNEL',None)
            else:os.environ['FUSE_X_TEST_KERNEL']=old
    def read(self,marker,seconds=180):
        if self.first_marker:
            marker,self.first_marker=self.first_marker,None
        return super().read(marker,seconds)
def image(work):
    stage=work/'stage';stage.mkdir()
    names=['native-hello','cat','ls','cp','sh','echo','ps','kill','mux','svcd','svc','console-session']
    for name in names:(stage/(name+'.fxe')).symlink_to(BUILD/'apps'/(name+'.fxe'))
    (stage/'sv-helper.fxe').symlink_to(OUT/'native/sv-helper.fxe');names.append('sv-helper')
    (work/'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
    subprocess.run(['node',str(ROOT/'fuse-x/image.mjs'),'create',str(work/'disk.img'),'--apps',str(stage),'--boot-profile','fuse-x'],check=True)
def update(work,files):
    source='''import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const files=JSON.parse(process.argv[2]).map(f=>({...f,data:Buffer.from(f.text||'')}));
atomicWrite(process.argv[1],installRoot(openImage(process.argv[1]),files));'''
    subprocess.run(['node','--input-type=module','-e',source,str(work/'disk.img'),json.dumps(files)],cwd=ROOT,check=True)
def read_file(work,path):
    source="import {openImage} from './ports/fuzix/tools/image.mjs';import {rootReader} from './ports/fuzix/tools/read-root.mjs';console.log(String.fromCharCode(...rootReader(openImage(process.argv[1])).read(process.argv[2])));"
    return subprocess.check_output(['node','--input-type=module','-e',source,str(work/'disk.img'),path],cwd=ROOT,text=True)
def service_files():
    files=[dict(name='/etc/boot/services.ini',text='[services]\nenabled=consumer\n')]
    for name,mode,requires in [('prepare','one',''),('provider','ready','prepare'),('consumer','ready','provider')]:
        text=f'[service]\nprogram=/bin/sv-helper\ntype={"oneshot" if mode=="one" else "simple"}\nrequires={requires}\n'
        if mode!='one':text+='ready=notify\n'
        text+=f'[arguments]\n1={mode}\n2=/service-events\n3={name}\n'
        files.append(dict(name='/etc/services/'+name+'.ini',mode=0o100644,text=text))
    return files
def state(g,name,wanted):
    until=time.monotonic()+20
    while time.monotonic()<until:
        output=g.shell('svc --runtime /var/run/svcd status '+name)
        if f'{name} {wanted} ' in output:return output
    raise AssertionError(output)
def close(g,label):
    try:g.close()
    finally:(OUT/('boot-'+label+'.log')).write_bytes(g.log)
with tempfile.TemporaryDirectory(prefix='sv-boot-',dir='/tmp') as temporary:
    work=Path(temporary);image(work);update(work,service_files())
    g=BootGuest(work,mux=True);term=Terminal(g);term.at=0;term.active=True
    try:
        term.shown('1:shell 1');term.line('echo INSIDE_MUX');term.shown('INSIDE_MUX');term.leave()
        state(g,'consumer','running');state(g,'prepare','done')
        assert 'OTHER_SHELL' in g.shell("sh -c 'echo OTHER_SHELL'")
        g.shell('svcd --boot');assert 'reserved for the initial PID-1 launch' in g.log.decode(errors='replace')
        entered=g.log.count(b'\x1b[?1049h');assert entered==1,entered
        os.write(g.master,b'exit\r');g.read(b'Fuse-X: shell stopped');g.read(g.prompt)
        assert 'RESPAWN_OK' in g.shell('echo RESPAWN_OK')
        assert g.log.count(b'\x1b[?1049h')==entered,'mux restarted after outer shell exit'
    finally:close(g,'mux')
    events=read_file(work,'/service-events').splitlines()
    assert events[:3]==['start prepare','start provider','start consumer'],events
    assert events.index('stop consumer')<events.index('stop provider'),events
    # The production installer must preserve edited console/service policies.
    update(work,[dict(name='/etc/boot/console.ini',text='[console]\nprogram=\n')])
    subprocess.run(['node',str(ROOT/'fuse-x/image.mjs'),'install',str(work/'disk.img'),'--apps',str(work/'stage'),'--boot-profile','fuse-x'],check=True)
    assert 'program=\n' in read_file(work,'/etc/boot/console.ini')
    assert 'enabled=consumer' in read_file(work,'/etc/boot/services.ini')
    # Abrupt supervisor loss leaves a stale socket; the console and PID 1 survive.
    g=BootGuest(work)
    try:
        state(g,'consumer','running')
        procs=g.shell('ps -Af');pid=None
        for line in procs.splitlines():
            fields=line.split()
            if fields and fields[-1]=='svcd':pid=int(fields[1]);break
        assert pid,procs
        g.shell('kill -9 '+str(pid))
        if b'console remains available' not in g.log:g.read(b'console remains available')
        assert 'SUPERVISOR_LOST' in g.shell('echo SUPERVISOR_LOST')
        assert not any(line.split()[-1:] == ['svcd'] for line in g.shell('ps -Af').splitlines())
    finally:close(g,'crash')
    # A genuine fresh boot may remove that socket; normal starts never do.
    g=BootGuest(work)
    try:state(g,'consumer','running')
    finally:close(g,'stale')
    # Restore mux policy, then rescue must bypass both services and console policy.
    update(work,[dict(name='/etc/boot/console.ini',text='[console]\nprogram=/bin/mux\n')])
    g=BootGuest(work,rescue=True)
    try:
        assert b'rescue mode' in g.log and b'\x1b[?1049h' not in g.log
        assert 'svcd' not in [line.split()[-1] for line in g.shell('ps -A').splitlines() if line.split()]
    finally:close(g,'rescue')
    # A crashed interactive startup program cannot strand the terminal in raw mode.
    update(work,[dict(name='/etc/boot/console.ini',text='[console]\nprogram=/bin/sv-helper\n[arguments]\n1=console\n2=/console-events\n3=once\n')])
    g=BootGuest(work)
    try:
        assert b'CONSOLE_RAW_EXIT' in g.log,g.log
        assert g.log.rfind(b'\x1b[?1003l')>g.log.rfind(b'CONSOLE_RAW_EXIT')
        assert g.log.rfind(b'\x1b[?1049l')>g.log.rfind(b'CONSOLE_RAW_EXIT')
        assert 'TERMINAL_RECOVERED' in g.shell('echo TERMINAL_RECOVERED')
    finally:close(g,'raw')
    # Invalid console configuration recovers to a shell; invalid service config
    # does not block the primary console or cause supervisor respawn storms.
    update(work,[dict(name='/etc/boot/console.ini',text='[console]\nunknown=value\n'),dict(name='/etc/boot/services.ini',text='[services]\nenabled=absent\n')])
    g=BootGuest(work)
    try:
        assert b'invalid console policy' in g.log
        assert 'FALLBACK_OK' in g.shell('echo FALLBACK_OK')
        assert 'invalid profile' in g.shell('cat /var/log/svcd.log')
    finally:close(g,'invalid')
print('SERVICES_BOOT_OK: first-console mux, detach/respawn shell, profile preservation, ordered shutdown, supervisor crash/orphans, stale-socket reboot, rescue, raw-terminal recovery and invalid-config fallback')
