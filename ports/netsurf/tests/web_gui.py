#!/usr/bin/env python3
"""Opt-in real-web graphical browser checks; all HTTP/TLS runs in the guest."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
from public_peer import Relay, ROOT
from guests import image,check_disk
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/fusede/tools'))
from build_theme import png,unpack565

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],default='native')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    parser.add_argument('--url',default='https://wiby.me/')
    parser.add_argument('--title',default='Wiby')
    parser.add_argument('--rvm-test-binary',type=Path,help='Reuse a compiled rvm/tests/netsurf test executable')
    args=parser.parse_args();args.build=args.build.resolve()
    output=args.build/'web-gui';output.mkdir(exist_ok=True)
    native=args.target=='native';suffix='.fxe' if native else '.rxe'
    build=ROOT/('fuse-x/build' if native else 'build/fuzix')
    apps={n:args.build/(n+suffix) for n in ['netsurf','htget']}
    apps.update({n:build/('apps/'+n+suffix if native else n+suffix) for n in ['nano-X','kill','echo','sh']})
    files=[dict(name='/etc/ssl/cert.pem',path=str(ROOT/'ports/tls/roots/cert.pem')),
           dict(name='/dev/gfx',mode=0o20600,device=0x400)]
    with tempfile.TemporaryDirectory(prefix='netsurf-web-gui-') as temporary:
        work=Path(temporary);peer=Relay(work/'nic')
        if native:
            apps['nxhostcheck']=build/'apps/nxhostcheck.fxe'
            old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(work/'frames'),NANOX_KEEP_TTY='1')
            try:g=Guest(work,'native',apps,files,peer,blocks=65535)
            finally:os.environ.clear();os.environ.update(old)
            try:
                g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
                g.shell('nano-X -p &');g.shell('s=$!')
                if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
                import shlex
                start=len(g.log)
                g.shell('netsurf --trace '+shlex.quote(args.url)+' &');g.shell('p=$!')
                if b'NETSURF_GUI_DOCUMENT' not in g.log[start:]:g.read(b'NETSURF_GUI_DOCUMENT',300)
                # The trace may arrive in a partial terminal read.
                deadline=time.monotonic()+15;item=None
                while time.monotonic()<deadline:
                    g.pump()
                    try:
                        windows=json.loads((work/'frames/windows.json').read_text())
                        item=next(w for w in windows if w['visible'] and w['title'].startswith('NetSurf: '+args.title))
                        data=(work/f"frames/{item['id']}.ppm").read_bytes()
                        match=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                        w,h=map(int,match.groups());rgb=data[match.end():]
                        if len(rgb)==w*h*3 and rgb[64*w*3:].count(b'\x00\x00\x00')>100:break
                    except (FileNotFoundError,StopIteration,json.JSONDecodeError,AttributeError):pass
                else:raise AssertionError('public page did not render text')
                png(output/'native-web.png',w,h,[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
                wid=int(re.search(rb'NETSURF_GUI_READY (\d+)',g.log).group(1))
                g.shell(f'nxhostcheck ctl close {wid}')
                assert 'BROWSER_STATUS:0' in g.shell('wait $p; echo BROWSER_STATUS:$?')
                g.shell('kill $s; wait $s')
            finally:
                (output/'native.log').write_bytes(g.log);g.close();peer.close()
        else:
            extra=[dict(name=f['name'],host=f['path']) if 'path' in f else f for f in files]
            extra += [dict(name='/dev/urandom',mode=0o20444,device=0x104),
                      dict(name='/dev/crypto',mode=0o20666,device=0x105),
                      dict(name='/etc/resolv.conf',data='nameserver 192.168.64.1\n')]
            disk=image('fuzix',build,apps,work,extra)
            stop=threading.Event();errors=[]
            def pump():
                try:
                    while not stop.is_set():peer.pump();time.sleep(.001)
                except BaseException as error:errors.append(error)
            thread=threading.Thread(target=pump);thread.start()
            try:
                command=([str(args.rvm_test_binary.resolve()),'--ignored','--nocapture'] if args.rvm_test_binary else
                         ['cargo','test','--release','-p','rvm','--test','netsurf','--','--ignored','--nocapture'])
                env=dict(os.environ,NETSURF_TEST_DISK=str(disk),NETSURF_TEST_OUTPUT=str(output),
                         NETSURF_RVM_BUILD=str(build),NETSURF_TEST_NET=str(work/'nic'),NETSURF_TEST_WEB_URL=args.url,
                         NETSURF_TEST_WEB_TITLE=args.title)
                env.pop('NETSURF_TEST_WEB_SEARCH',None)
                if args.url in ['http://wiby.me/','https://wiby.me/']:env['NETSURF_TEST_WEB_SEARCH']='1'
                subprocess.run(command,
                    cwd=ROOT,check=True,env=env)
            finally:stop.set();thread.join();peer.close()
            assert not errors,errors
            check_disk(disk,False)
            import struct
            for source in output.glob('rvm-web*.rgb565'):
                data=source.read_bytes();words=struct.unpack('<'+'H'*(len(data)//2),data)
                png(source.with_suffix('.png'),640,480,[(*unpack565(p),255) for p in words])
        assert not peer.failures,peer.failures
    print(args.target+': public graphical browsing and clean filesystem passed')

if __name__=='__main__':main()
