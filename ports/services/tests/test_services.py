#!/usr/bin/env python3
"""Private, unprivileged service-manager checks; no system init/image is changed."""
from pathlib import Path
import os
import socket
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[3]
HERE=ROOT/'ports/services'
BIN=ROOT/'build/services/host'
subprocess.run([sys.executable,str(HERE/'build.py')],check=True)
compiler=['/usr/bin/clang' if sys.platform=='darwin' else 'cc','-std=c99','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
if sys.platform=='darwin':compiler+=['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
subprocess.run([*compiler,str(HERE/'tests/helper.c'),'-o',str(BIN/'helper')],check=True)
subprocess.run([*compiler,'-I'+str(ROOT/'libraries/ini'),str(HERE/'tests/memory.c'),str(ROOT/'libraries/ini/ini.c'),'-o',str(BIN/'memory-test')],check=True)

def wait(predicate,description,seconds=8):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if predicate():return
        time.sleep(.025)
    raise AssertionError(description)


with tempfile.TemporaryDirectory(prefix='sv-console-',dir='/tmp') as temporary:
    policy=Path(temporary)/'console.ini'
    for text in ('[console]\nprogram=\n','[console]\nprogram=/bin/mux\n[arguments]\n1=literal argument\n'):
        policy.write_text(text)
        subprocess.run([str(BIN/'console-session'),'--config',str(policy),'--check'],check=True)
    for text in ('[console]\nprogram=relative\n','[console]\nprogram=/bin/mux\nprogram=/bin/sh\n',
                 '[console]\nunknown=value\n','[console]\nprogram=/bin/mux\n[arguments]\n2=gap\n',
                 '[console]\nprogram=\n[arguments]\n1=no-program\n'):
        policy.write_text(text)
        assert subprocess.run([str(BIN/'console-session'),'--config',str(policy),'--check'],capture_output=True).returncode
    assert subprocess.run([str(BIN/'svcd'),'--boot'],capture_output=True).returncode

with tempfile.TemporaryDirectory(prefix='sv-',dir='/tmp') as temporary:
    work=Path(temporary);defs=work/'defs';defs.mkdir();runtime=work/'run';events=work/'events'
    empty=subprocess.run([str(BIN/'svcd'),'--config',str(defs),'--check'],capture_output=True,text=True,check=True)
    assert not empty.stdout
    def definition(name,mode,extra='',requires='',after='',program=None):
        text=f'[service]\nprogram={program or BIN/"helper"}\ntype={"oneshot" if mode in ("one","fail") else "simple"}\n'
        if mode not in ('one','fail'):text+='ready=notify\n'
        text+=f'requires={requires}\nafter={after}\n{extra}\n[arguments]\n1={mode}\n2={events}\n3={name}\n'
        (defs/(name+'.ini')).write_text(text)
    definition('prepare','one')
    definition('server','ready',requires='prepare')
    definition('dependent','ready',requires='server')
    definition('independent','ready',after='server')
    # A failed initialization task must finish before dependents are eligible.
    definition('failure','fail',extra='restart=on-failure\nrestart_limit=2\nrestart_delay_ms=100')
    definition('blocked','one',requires='failure')
    definition('timeout','no-ready',extra='start_timeout_ms=150')
    definition('stubborn','ignore',extra='stop_timeout_ms=150')
    definition('missing','one',program='/not/a/program')
    definition('literal','one')
    literal=defs/'literal.ini';literal.write_text(literal.read_text()+"4=hello Ж; $(touch SHOULD_NOT_EXIST)\n")
    base=[str(BIN/'svcd'),'--config',str(defs)]
    check=subprocess.run(base+['--check'],capture_output=True,text=True,check=True)
    assert all('disabled' in line for line in check.stdout.splitlines()),check.stdout
    assert not events.exists()
    # Planning rejects unknown keys, duplicate keys, missing dependencies, cycles,
    # sparse arguments, and writable configuration before launching anything.
    bad=defs/'bad.ini'
    for text in ('[service]\nprogram=/bin/true\nmystery=yes\n','[service]\nprogram=/bin/true\nprogram=/bin/false\n',
                 '[service]\nprogram=/bin/true\nrequires=absent\n','[service]\nprogram=/bin/true\nafter=bad\n',
                 '[service]\nprogram=/bin/true\n[arguments]\n2=gap\n'):
        bad.write_text(text);assert subprocess.run(base+['--check'],capture_output=True).returncode
    bad.unlink()
    os.chmod(literal,0o666);assert subprocess.run(base+['--check'],capture_output=True).returncode;os.chmod(literal,0o644)
    profile=work/'profile.ini';profile.write_text('[services]\nenabled=dependent\n')
    plan=subprocess.run(base+['--check','--profile',str(profile)],capture_output=True,text=True,check=True).stdout
    assert 'prepare enabled' in plan and 'server enabled' in plan and 'dependent enabled' in plan and 'independent disabled' in plan,plan
    log=(work/'manager.log').open('w+')
    manager=subprocess.Popen(base+['--runtime',str(runtime)],stdout=log,stderr=log)
    def command(verb,name=None,ok=True):
        result=subprocess.run([str(BIN/'svc'),'--runtime',str(runtime),verb,*([name] if name else [])],capture_output=True,text=True,timeout=7)
        if ok:assert result.returncode==0,result.stdout+result.stderr
        return result.stdout
    def state(name,value):return f'{name} {value} ' in command('status',name)
    try:
        wait(lambda:(runtime/'control.sock').exists(),'manager ready')
        time.sleep(.15);assert not events.exists(),'definitions unexpectedly auto-started'
        duplicate=subprocess.run(base+['--runtime',str(runtime)],capture_output=True,timeout=5)
        assert duplicate.returncode and manager.poll() is None
        command('start','dependent');wait(lambda:state('dependent','running'),'requires and readiness')
        assert events.read_text().splitlines()[:3]==['start prepare','start server','start dependent']
        assert state('independent','down'),'after must not enable a service'
        command('start','independent');wait(lambda:state('independent','running'),'after-only service')
        command('start','literal');wait(lambda:state('literal','done'),'literal argument completion')
        assert 'hello Ж; $(touch SHOULD_NOT_EXIST)' in (runtime/'literal.log').read_text()
        assert not Path('/SHOULD_NOT_EXIST').exists()
        command('restart','server');wait(lambda:events.read_text().count('start server')==2,'restart service')
        wait(lambda:state('dependent','running'),'dependent recovery after provider restart')
        command('stop','server');wait(lambda:state('server','down') and state('dependent','down'),'stop required subtree')
        assert state('independent','running'),'after must not impose lifetime dependency'
        lines=events.read_text().splitlines();assert lines.index('stop dependent')<lines.index('stop server'),lines
        command('start','blocked');wait(lambda:state('failure','failed') and state('blocked','blocked'),'restart bound and blocked dependency')
        assert 'retries=2' in command('status','failure')
        command('start','missing');wait(lambda:state('missing','failed'),'exec failure must not become done')
        command('start','timeout');wait(lambda:state('timeout','failed'),'notify timeout')
        command('start','stubborn');wait(lambda:state('stubborn','running'),'stubborn readiness');command('stop','stubborn')
        wait(lambda:state('stubborn','down'),'TERM deadline escalates to KILL')
        assert 'signal=9' in command('status','stubborn')
        # A partial control request cannot suspend supervision; it expires.
        slow=socket.socket(socket.AF_UNIX);slow.connect(str(runtime/'control.sock'));slow.sendall(b'sta')
        time.sleep(2.2);slow.close();assert 'OK' in command('status')
        assert command('start','unknown',ok=False).startswith('ERR')
        command('shutdown');manager.wait(timeout=5);assert manager.returncode==0 and not (runtime/'control.sock').exists()
        # Explicit profile activation is the only autostart path.
        profile.write_text('[services]\nenabled=dependent independent\n')
        manager=subprocess.Popen(base+['--runtime',str(runtime),'--profile',str(profile)],stdout=log,stderr=log)
        wait(lambda:(runtime/'control.sock').exists(),'profile manager')
        wait(lambda:state('dependent','running'),'explicit profile closure')
        wait(lambda:state('independent','running'),'profile ordering-only service')
        before=len(events.read_text().splitlines());command('shutdown');manager.wait(timeout=5)
        tail=events.read_text().splitlines()[before:]
        assert tail.index('stop server')>tail.index('stop dependent') and tail.index('stop server')>tail.index('stop independent'),tail
        print('SERVICES_OK: disabled defaults, validation, dependency planning, literal argv, readiness, restart limits, stop order, timeouts, IPC and explicit profiles')
    finally:
        if manager.poll() is None:
            manager.terminate()
            try:manager.wait(timeout=5)
            except subprocess.TimeoutExpired:manager.kill();manager.wait()
        log.seek(0)
        if sys.exc_info()[0]:print(log.read())
        log.close()

# Cross both 32/64-bit mask boundaries and the former 4096-cell status buffer.
with tempfile.TemporaryDirectory(prefix='sv-large-',dir='/tmp') as temporary:
    work=Path(temporary);defs=work/'defs';defs.mkdir();runtime=work/'run';events=work/'events'
    names=[f'definition{i:03}' for i in range(100)]
    def write_definition(index,requires='',after=''):
        mode='ready' if index>=98 else 'one'
        text=f'[service]\nprogram={BIN/"helper"}\ntype={"simple" if mode=="ready" else "oneshot"}\n'
        if mode=='ready':text+='ready=notify\n'
        text+=f'requires={requires}\nafter={after}\n[arguments]\n1={mode}\n2={events}\n3={names[index]}\n'
        (defs/(names[index]+'.ini')).write_text(text)
    for index in range(100):write_definition(index)
    # More than 512 cells in a dependency list; reverse-alphabetic edge order.
    write_definition(97,requires=' '.join(reversed(names[:96])))
    write_definition(96,after=names[97])
    write_definition(99,requires=names[98])
    profile=work/'profile.ini';profile.write_text('[services]\nenabled='+','.join(names)+'\n')
    base=[str(BIN/'svcd'),'--config',str(defs)]
    plan=subprocess.run(base+['--check','--profile',str(profile)],capture_output=True,text=True,check=True).stdout.splitlines()
    assert len(plan)==100 and all(line.endswith(' enabled') for line in plan),plan
    assert plan.index(names[97]+' enabled')<plan.index(names[96]+' enabled'),plan
    write_definition(98,requires=names[99])
    invalid=subprocess.run(base+['--check'],capture_output=True,text=True)
    assert invalid.returncode and 'cycle' in invalid.stderr,invalid
    write_definition(98)
    subprocess.run([str(BIN/'memory-test'),str(defs),str(profile)],check=True)
    log=(work/'manager.log').open('w+')
    manager=subprocess.Popen(base+['--runtime',str(runtime)],stdout=log,stderr=log)
    try:
        wait(lambda:(runtime/'control.sock').exists(),'large manager ready')
        output=command('status')
        assert len(output)>4096 and len(output.splitlines())==101,output
        assert all(f'{name} down ' in output for name in names),output
        assert not events.exists()
        command('start',names[97]);wait(lambda:state(names[97],'done'),'large dependency fan-in',seconds=25)
        output=command('status')
        assert all(f'{name} done ' in output for name in names[:96]),output
        assert state(names[96],'down'),'after-only consumer must stay disabled'
        command('start',names[99]);wait(lambda:state(names[99],'running'),'high-index readiness')
        command('restart',names[98]);wait(lambda:events.read_text().splitlines().count('start '+names[98])==2,'high-index restart')
        wait(lambda:state(names[99],'running'),'high-index recovery')
        command('stop',names[98]);wait(lambda:state(names[98],'down') and state(names[99],'down'),'high-index stop closure')
        command('shutdown');manager.wait(timeout=5);assert manager.returncode==0
        print('SERVICES_LARGE_OK: 100 definitions, long profile/dependency lists, full streamed status, graph validation and high-index lifecycle')
    finally:
        if manager.poll() is None:
            manager.terminate()
            try:manager.wait(timeout=5)
            except subprocess.TimeoutExpired:manager.kill();manager.wait()
        log.seek(0)
        if sys.exc_info()[0]:print(log.read())
        log.close()
