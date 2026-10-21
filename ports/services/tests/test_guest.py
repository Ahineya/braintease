#!/usr/bin/env python3
"""Manually launch synthetic services in a private Fuse-X/RVM image only."""
import argparse
from pathlib import Path
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=['native','ripple'],default='native')
a=p.parse_args()
build=ROOT/'build/services'/a.target
ext='fxe' if a.target=='native' else 'rxe'
apps={name:build/(name+'.'+ext) for name in ('svcd','svc','sv-helper')}
files=[]
# Failed initialization uses oneshot: exec readiness alone permits dependents
# before a short-lived simple service exits, which would make this test race.
for name,mode,requires,extra in [('prepare','one','',''),('server','ready','prepare','ready=notify'),('dependent','ready','server','ready=notify'),('failure','fail','','restart=on-failure\nrestart_limit=2\nrestart_delay_ms=100'),('blocked','one','failure',''),('timeout','no-ready','','ready=notify\nstart_timeout_ms=200')]:
    text=f'[service]\nprogram=/bin/sv-helper\ntype={"oneshot" if mode in ("one","fail") else "simple"}\nrequires={requires}\n{extra}\n[arguments]\n1={mode}\n2=/service-events\n3={name}\n'
    files.append(dict(name=f'/svdefs/{name}.ini',mode=0o100644,text=text))
# Keep over 64 definitions installed, but start only a few synthetic processes.
# These names sort after the original fixtures, exercising indices beyond masks.
for index in range(70):
    name=f'unit{index:03}'
    text='[service]\nprogram=/bin/sv-helper\ntype=oneshot\n'
    if index==69:text+='requires=unit068\n'
    text+=f'[arguments]\n1=one\n2=/service-events\n3={name}\n'
    files.append(dict(name=f'/svdefs/{name}.ini',mode=0o100644,text=text))
with tempfile.TemporaryDirectory(prefix='service-guest-') as temporary:
    guest=Guest(Path(temporary),a.target,apps,files,blocks=65536)
    def command(verb,name=''):
        return guest.shell(f'svc --runtime /svrun {verb} {name}')
    def state(name,wanted):
        end=time.monotonic()+40
        while time.monotonic()<end:
            text=command('status',name)
            if f'{name} {wanted} ' in text:return text
        raise AssertionError(name+' did not reach '+wanted+'\n'+text)
    try:
        output=guest.shell('svcd --check --config /svdefs');assert 'server disabled' in output,output
        guest.shell('svcd --config /svdefs --runtime /svrun > /sv-manager.log 2>&1 &')
        state('server','down')
        output=command('status')
        assert len(output)>4096 and all(f'unit{i:03} down ' in output for i in range(70)),output
        command('start','unit069');state('unit069','done');state('unit068','done')
        command('stop','unit068');state('unit069','down');state('unit068','down')
        assert 'OK' in command('start','dependent')
        state('dependent','running');state('prepare','done')
        events=guest.shell('cat /service-events')
        assert events.index('start prepare')<events.index('start server')<events.index('start dependent'),events
        command('restart','server')
        until=time.monotonic()+40
        while time.monotonic()<until:
            if guest.shell('cat /service-events').count('start server')>=2:break
        else:raise AssertionError('server did not restart')
        state('server','running');state('dependent','running')
        command('stop','server');state('dependent','down');state('server','down')
        events=guest.shell('cat /service-events');assert events.index('stop dependent')<events.index('stop server'),events
        command('start','blocked');assert 'retries=2' in state('failure','failed');state('blocked','blocked')
        command('start','timeout');state('timeout','failed')
        assert 'OK' in command('shutdown')
        print(a.target+': 76 definitions, streamed status, high-index dependencies, disabled defaults, readiness, restart, ordered stop, crash limits and readiness timeout passed',flush=True)
    finally:
        # Every process belongs to the disposable VM; no host daemon is started.
        (ROOT/'build/services'/(a.target+'-guest.log')).write_bytes(guest.log)
        guest.close()
