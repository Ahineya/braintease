#!/usr/bin/env python3
"""Interrupt/budget shutdown must release listeners so a fresh process can bind."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/fuzix/tools'))
from network_peer import Peer

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('host','native','ripple'),required=True)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--build',type=Path,default=ROOT/'build/quickjs-server-tests')
a=p.parse_args();binary=a.binary.resolve();build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
report=build/(a.target+'-lifecycle-report.json');report.unlink(missing_ok=True)
with tempfile.TemporaryDirectory(prefix='qjs-server-lifecycle-') as temporary:
    directory=Path(temporary);peer=guest=None
    host='127.0.0.1' if a.target=='host' else '0.0.0.0'
    server="import {listen} from 'fuse:net';const l=listen({hostname:"+json.dumps(host)+",port:0}); console.log('LISTENER_READY',l.address.port);await l.accept({timeoutMs:2147483647});"
    probe="import {listen} from 'fuse:net';const l=listen({hostname:"+json.dumps(host)+",port:Number(scriptArgs[1])});console.log('REBIND_OK');l.close();"
    if a.target=='host':
        (directory/'listener.mjs').write_text(server);(directory/'probe.mjs').write_text(probe)
    else:
        peer=Peer(directory/'nic');guest=Guest(directory,a.target,{'js':binary},peer=peer,blocks=65536,
            files=[dict(name='/tmp/listener.mjs',text=server),dict(name='/tmp/probe.mjs',text=probe)])
    try:
        for mode in ('signal','budget'):
            args=['--server']+(['--max-time-ms','5000'] if mode=='budget' else [])
            process=None;output=bytearray()
            try:
                if guest:
                    import shlex
                    os.write(guest.master,(shlex.join(['js',*args,'/tmp/listener.mjs'])+'; echo LIFE_STATUS:$?\r').encode())
                    text=guest.read(b'\nLISTENER_READY ',120)+guest.read(b'\n',30)
                    port=int(re.search(r'LISTENER_READY (\d+)',text)[1])
                    if mode=='signal':os.write(guest.master,b'\x03')
                    output=guest.read(guest.prompt,60)
                    assert 'LIFE_STATUS:'+('130' if mode=='signal' else '1') in output,output
                    assert 'REBIND_OK' in guest.shell('js /tmp/probe.mjs '+str(port),120)
                else:
                    process=subprocess.Popen([str(binary),*args,str(directory/'listener.mjs')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                    until=time.monotonic()+30
                    while b'\n' not in output:
                        if select.select([process.stdout],[],[],.05)[0]:output.extend(os.read(process.stdout.fileno(),65536))
                        assert process.poll() is None and time.monotonic()<until,output
                    port=int(re.search(rb'LISTENER_READY (\d+)',output)[1])
                    if mode=='signal':process.send_signal(signal.SIGINT)
                    rest,_=process.communicate(timeout=30);output.extend(rest)
                    assert process.returncode==(130 if mode=='signal' else 1),output
                    result=subprocess.run([str(binary),str(directory/'probe.mjs'),str(port)],capture_output=True,timeout=30)
                    assert result.returncode==0 and b'REBIND_OK' in result.stdout,(result.stdout,result.stderr)
            finally:
                if process and process.poll() is None:process.kill();process.wait()
            print(a.target+': '+mode+' listener cleanup passed',flush=True)
    finally:
        if guest:guest.close()
        if peer:peer.close()
report.write_text(json.dumps(dict(status='passed',target=a.target,cases=['SIGINT listener cleanup and rebind','wall-budget listener cleanup and rebind'],executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()),indent=2)+'\n')
