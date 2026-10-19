#!/usr/bin/env python3
"""Lynx HTTPS through the real guest stack, including interactive cancellation."""
import argparse
import importlib.util
import os
from pathlib import Path
import tempfile
import time
from fixtures import make
from guest import Guest,ROOT
from peer import Services

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('native','ripple'),default='native')
p.add_argument('--executable',type=Path)
p.add_argument('--smoke',action='store_true')
p.add_argument('--ui-only',action='store_true')
a=p.parse_args()
exe=a.executable or ROOT/'build/tls-accel'/('lynx.fxe' if a.target=='native' else 'lynx.rxe')
HTML=b'<html><title>Guest HTTPS browser</title><h1>Verified HTTPS in Lynx</h1><p>Guest TLS and Nano-X terminal work together.</p><a href="https://tls.test:444/chunked">TLS 1.2 link</a></html>'
with tempfile.TemporaryDirectory(prefix='ripple-lynx-https-') as temporary:
    directory=Path(temporary);fixtures=make(directory/'certs')
    peer=Services(directory/'nic',fixtures,HTML,b'text/html')
    files=[dict(name='/etc/ssl/cert.pem',path=str(fixtures/'ca.pem')),
           dict(name='/other-root',path=str(fixtures/'other.pem')),
           dict(name='/etc/lynx.cfg',path=str(ROOT/'ports/lynx/fixtures/lynx.cfg')),
           dict(name='/etc/termcap',text='ansi|vt100|xterm|xterm-256color:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')]
    guest=Guest(directory,a.target,{'lynx':exe},files,peer,blocks=65536)
    try:
        urls=['https://tls.test/binary'] if a.smoke else [
            'https://tls.test/binary','https://tls.test:444/binary','https://tls.test:446/binary',
            'https://tls.test:447/binary','https://tls.test:448/binary','https://tls.test/redirect','https://tls.test/chunked']
        for url in ([] if a.ui_only else urls):
            text=guest.shell(f'lynx -dump -nolist -cfg=/etc/lynx.cfg {url}; echo LYNX_STATUS:$?',300)
            print(text,flush=True)
            assert 'LYNX_STATUS:0' in text and 'Verified HTTPS in Lynx' in text,text
            assert '<h1>' not in text,text
        if not a.smoke:
            for url,env in ([] if a.ui_only else [('https://wrong.test/binary',''),('https://tls.test:445/binary',''),
                    ('https://tls.test:451/binary',''),('https://tls.test:452/binary',''),
                    ('https://tls.test/binary','SSL_CERT_FILE=/other-root ')]):
                text=guest.shell(f'{env}lynx -dump -nolist -cfg=/etc/lynx.cfg {url}; echo BAD_STATUS:$?',300)
                print(text,flush=True)
                assert 'BAD_STATUS:1' in text and 'Verified HTTPS in Lynx' not in text,text
            # Reuse the terminal decoder without confusing the two guest modules.
            spec=importlib.util.spec_from_file_location('lynx_ui',ROOT/'ports/lynx/guest.py')
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            screen=module.Screen();at=len(guest.log)
            def visible(marker):
                global at
                until=time.monotonic()+180
                while time.monotonic()<until:
                    guest.pump();screen.feed(bytes(guest.log[at:]));at=len(guest.log)
                    if marker in screen.text():return
                raise AssertionError((marker,screen.text()))
            os.write(guest.master,b'lynx -cfg=/etc/lynx.cfg https://tls.test/binary; echo UI_STATUS:$?\r')
            visible('Verified HTTPS in Lynx')
            before=len(peer.paths)
            os.write(guest.master,b'\x1b[C')
            until=time.monotonic()+180
            while '/chunked' not in peer.paths[before:]:
                guest.pump();assert time.monotonic()<until,'No TLS navigation'
            visible('Commands:')
            visible('Verified HTTPS in Lynx')
            # Go to a connection whose response body never completes, then cancel.
            os.write(guest.master,b'g');visible('URL to open')
            os.write(guest.master,b'\x15https://tls.test/hang\r')
            until=time.monotonic()+180
            while '/hang' not in peer.paths:
                guest.pump();assert time.monotonic()<until,'No hanging request'
            os.write(guest.master,b'z');visible('Commands:')
            os.write(guest.master,b'q');visible('Are you sure');os.write(guest.master,b'y')
            text=guest.read(guest.prompt,90);print('Lynx interactive HTTPS returned to the shell',flush=True)
            assert 'UI_STATUS:0' in text,text
    finally:guest.close();peer.close()
print('Lynx verified HTTPS rendering smoke passed' if a.smoke else 'Lynx interactive HTTPS lifecycle passed' if a.ui_only else 'Lynx HTTPS rendering, certificate rejection and interactive lifecycle passed')
