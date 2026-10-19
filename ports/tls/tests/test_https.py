#!/usr/bin/env python3
"""Authenticated HTTPS interoperability over guest TCP, using an OpenSSL peer."""
import argparse
import os
import time
from pathlib import Path
import tempfile
from fixtures import make
from guest import Guest, ROOT
from peer import Services, BINARY

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('native','ripple'),default='native')
p.add_argument('--executable',type=Path)
p.add_argument('--smoke',action='store_true')
p.add_argument('--url',action='append',help='run only the specified positive download(s)')
p.add_argument('--negative-only',action='store_true')
p.add_argument('--lifecycle-only',action='store_true',help='check cancellation and missing entropy only')
a=p.parse_args()
exe=a.executable or ROOT/'build/tls'/('htget.fxe' if a.target=='native' else 'htget.rxe')
with tempfile.TemporaryDirectory(prefix='ripple-https-') as directory:
    directory=Path(directory)
    fixtures=make(directory/'fixtures')
    expected=directory/'expected';expected.write_bytes(BINARY)
    peer=Services(directory/'nic',fixtures)
    build=ROOT/('fuse-x/build/apps' if a.target=='native' else 'build/fuzix')
    suffix='.fxe' if a.target=='native' else '.rxe'
    files=[dict(name='/etc/ssl/cert.pem',mode=0o100644,path=str(fixtures/'ca.pem')),
           dict(name='/other-root',path=str(fixtures/'other.pem')),
           dict(name='/public-roots',path=str(ROOT/'ports/tls/roots/cert.pem')),
           dict(name='/download',path=str(expected)),
           dict(name='/expected',path=str(expected))]
    guest=Guest(directory,a.target,{'htget':exe,'cmp':build/('cmp'+suffix),'rm':build/('rm'+suffix)},files,peer)
    try:
        for index,url in enumerate([] if a.negative_only or a.lifecycle_only else a.url or (['https://tls.test/binary'] if a.smoke else [
                'https://tls.test/binary','https://tls.test:444/binary','https://tls.test/chunked',
                'https://tls.test/redirect','https://192.168.64.1/binary',
                'https://tls.test:446/binary','https://tls.test:447/binary',
                'https://tls.test:448/binary','https://tls.test:449/binary',
                'https://tls.test:450/binary'])):
            started=time.monotonic()
            text=guest.shell(f'htget {url} /download; echo HTTPS_STATUS:$?',300)
            print(text,f'Download elapsed: {time.monotonic()-started:.2f}s',flush=True)
            assert 'HTTPS_STATUS:0' in text,(text,peer.failures,peer.sni,peer.versions,peer.paths,[(s.local,s.remote,len(s.received),len(s.pending),s.fin,s.fin_sent,s.window,s.flight) for s in peer.streams.values()])
            text=guest.shell('cmp /expected /download; echo BODY_STATUS:$?')
            assert 'BODY_STATUS:0' in text,text
        if not a.smoke and not a.url:
            for arguments,diagnostic in ([] if a.lifecycle_only else [
                        ('https://wrong.test/binary','TLS error -322'),
                        ('https://tls.test:445/binary','TLS error -151'),
                        ('https://tls.test:451/binary','TLS error -322'),
                        ('https://tls.test:452/binary','TLS error -322'),
                        ('https://tls.test/truncated','TLS error -397'),
                        ('https://tls.test/unclean','TLS error -397'),
                        ('https://tls.test/tampered','TLS error -180'),
                        ('https://tls.test/downgrade','invalid, insecure or excessive redirect'),
                        ('--cacert /other-root https://tls.test/binary','TLS error -188'),
                        ('--cacert /public-roots https://tls.test/binary','TLS error -188')]):
                text=guest.shell(f'htget {arguments} /download; echo BAD_STATUS:$?',300)
                print(text,flush=True)
                assert 'BAD_STATUS:1' in text,text
                assert diagnostic in text,(text,peer.failures,[(s.local,s.remote,len(s.received),len(s.pending),s.fin,s.fin_sent,s.window,s.flight) for s in peer.streams.values()])
                text=guest.shell('cmp /expected /download; echo PRESERVED:$?')
                assert 'PRESERVED:0' in text,text
                if '/public-roots' in arguments:
                    assert 'cannot load trusted certificates' not in text,text
            # Interrupt a valid connection after its headers arrive, while it waits
            # for a close-delimited body. The shell must survive, with the old file.
            os.write(guest.master,b'htget https://tls.test/hang /download; echo CANCEL_STATUS:$?\r')
            until=time.monotonic()+180
            while '/hang' not in peer.paths:
                guest.pump()
                assert time.monotonic()<until,'No hanging request reached the TLS peer'
            os.write(guest.master,b'\x03')
            text=guest.read(guest.prompt,60)
            print(text,flush=True)
            assert 'CANCEL_STATUS:130' in text,text
            assert 'PRESERVED:0' in guest.shell('cmp /expected /download; echo PRESERVED:$?')
            # A fresh process must fail closed without an OS entropy source.
            text=guest.shell('rm /dev/urandom; htget https://tls.test/binary /download; echo RNG_STATUS:$?',300)
            print(text,flush=True)
            assert 'RNG_STATUS:1' in text,text
            assert 'cannot seed TLS from /dev/urandom' in text,text
            assert 'PRESERVED:0' in guest.shell('cmp /expected /download; echo PRESERVED:$?')
            if not a.negative_only and not a.lifecycle_only:
                assert {'TLSv1.2','TLSv1.3'}<=set(peer.versions),peer.versions
        assert 'tls.test' in peer.sni,peer.sni
    finally:
        guest.close();peer.close()
print('Verified guest HTTPS, OpenSSL interoperability, binary bodies and clean shutdown passed')
