#!/usr/bin/env python3
"""Validate real C16 package creation, loading and signature verification."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from peer import HTTPServices

def main():
    engine=ROOT/'build/pkg/pkg-engine.fxe'; host=ROOT/'build/pkg/host/pkg-engine'
    if not engine.exists():raise SystemExit('Build ports/pkg/build_guest.py first')
    with tempfile.TemporaryDirectory(prefix='pkg-guest-') as temporary:
        work=Path(temporary).resolve(); stage=work/'stage'; (stage/'usr/bin').mkdir(parents=True)
        script=b'#!/bin/sh\necho PACKAGE_GUEST_OK\n'
        (stage/'usr/bin/pkghello').write_bytes(script);(stage/'usr/bin/pkghello').chmod(0o755)
        meta=work/'manifest'; meta.write_text('name=pkghello\nversion=1\ntarget=any\ndepends=\n')
        archive=work/'hello.rpk'
        subprocess.run([str(host),'create',str(meta),str(stage),str(archive)],check=True)
        expected=hashlib.sha256(archive.read_bytes()).hexdigest()
        repo=work/'repo';repo.mkdir();(repo/'hello.rpk').write_bytes(archive.read_bytes())
        manager=ROOT/'build/pkg/packages/pkg-fuse-x.rpk'
        if manager.exists():(repo/'manager.rpk').write_bytes(manager.read_bytes())
        # Real test package, including the '[' alias and its library closure.
        catalog=json.loads((ROOT/'build/pkg/catalog/fuse-x/catalog.json').read_text())
        packages={p['name']:p for p in catalog['packages']};included=set()
        def include(name):
            if name in included:return
            included.add(name);p=packages[name]
            for dep in p['depends']:include(dep.split('@')[0])
            source=Path(p['archive']);(repo/source.name).write_bytes(source.read_bytes())
        include('test')
        subprocess.run([str(host),'repo',str(repo)],check=True)
        private=work/'key.pem'
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(private)],check=True,capture_output=True)
        modulus=subprocess.check_output(['openssl','rsa','-in',str(private),'-noout','-modulus']).decode().strip().split('=')[1].lower()+'\n'
        subprocess.run(['openssl','dgst','-sha256','-sign',str(private),'-out',str(repo/'index.sig'),str(repo/'index.v1')],check=True)
        blob=bytes(range(256))*9;(work/'blob').write_bytes(blob)
        class RepoPeer(HTTPServices):
            def http(self, stream):
                data=bytes(stream.received)
                if getattr(stream,'answered',False) or b'\r\n\r\n' not in data:return
                stream.answered=True
                path=data.split(b' ')[1].decode();stream.path=path
                self.paths.append(path)
                allowed={'/index.v1':repo/'index.v1','/index.sig':repo/'index.sig'}
                allowed.update({'/objects/'+p.name:p for p in (repo/'objects').iterdir()})
                if path not in allowed:raise AssertionError(path)
                body=allowed[path].read_bytes();stream.chunk=1024
                stream.queue(b'HTTP/1.0 200 OK\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
        peer=RepoPeer(work/'nic')
        files=[dict(name='/etc/pkg/repo.conf',text='url=http://repo.test\n'),dict(name='/etc/pkg/repo.pub',text=modulus),dict(name='/stage/usr/bin/pkghello',mode=0o100755,text=script.decode()),
               dict(name='/manifest',text=meta.read_text()),dict(name='/blob',path=str(work/'blob')),
               dict(name='/index.v1',path=str(repo/'index.v1')),dict(name='/index.sig',path=str(repo/'index.sig')),
               dict(name='/repo.pub',text=modulus)]
        apps={'pkg':engine}
        for tool in ('mkdir','cp','chmod','cmp','dirname','pwd','test','rm'):
            apps[tool]=ROOT/'fuse-x/build/apps'/(tool+'.fxe')
        files.extend([dict(name='/bootstrap/install.sh',path=str(ROOT/'tools/packages/bootstrap.sh')),dict(name='/bootstrap/pkg-engine',mode=0o100755,path=str(engine)),dict(name='/bootstrap/repo.pub',text=modulus)])
        frontend=ROOT/'build/pkg/pkg.fxe'
        if frontend.exists():apps.update({'pkgui':frontend})
        guest=Guest(work,'native',apps,files=files,blocks=32768,peer=peer)
        try:
            def run(command,marker=None):
                output=guest.shell(command)
                if marker and marker not in output:raise AssertionError(output)
                if 'pkg:' in output:raise AssertionError(output)
                return output
            run('pkg hash /blob',hashlib.sha256(blob).hexdigest())
            run('pkg create /manifest /stage /hello.rpk')
            run('pkg hash /hello.rpk',expected)
            run('pkg install /hello.rpk')
            run('/usr/bin/pkghello','PACKAGE_GUEST_OK')
            run('pkg list','pkghello 1 [any]')
            run('pkg remove pkghello');run('pkg clean')
            run('mkdir -p /var/lib/pkg/stage/root')
            run('echo partial > /var/lib/pkg/stage/root/partial')
            run('pkg recover')
            run('test ! -d /var/lib/pkg/stage && echo RECOVERED','RECOVERED')
            run('pkg update','Verified repository index:')
            run('pkg add pkghello','Downloading pkghello')
            run('/usr/bin/pkghello','PACKAGE_GUEST_OK')
            run('pkg remove pkghello')
            if manager.exists():
                assert 'not found' in guest.shell('[ yes ]')
                run('rm /etc/pkg/repo.conf /etc/pkg/repo.pub')
                run('sh /bootstrap/install.sh http://repo.test','Installed pkg.')
                run('sh /bootstrap/install.sh','Installed pkg.')
                run('/bin/[ -f /etc/pkg/repo.conf ] && echo BRACKET_FILE_OK','BRACKET_FILE_OK')
                run('/usr/bin/[ 3 -gt 2 ] && echo BRACKET_NUMBER_OK','BRACKET_NUMBER_OK')
                run('if /usr/bin/[ 2 -gt 3 ]; then echo BAD; else echo BRACKET_FALSE_OK; fi','BRACKET_FALSE_OK')
                assert "Missing ']'" in guest.shell('/usr/bin/[ yes')
                run('/usr/bin/pkg list','pkg 0.')
                run('/usr/bin/pkg add pkghello','Downloading pkghello')
                run('/usr/bin/pkghello','PACKAGE_GUEST_OK')
            assert '/index.sig' in peer.paths and any(p.startswith('/objects/') for p in peer.paths)
            run('pkg verify /index.v1 /index.sig /repo.pub')
            run('RIPPLE_TLS_SOFTWARE=1 pkg verify /index.v1 /index.sig /repo.pub')
            if frontend.exists():run('PKG_ENGINE=/bin/pkg pkgui inspect /hello.rpk','pkghello 1 [any]')
            assert 'repository signature rejected' in guest.shell('echo bad > /index.v1; pkg verify /index.v1 /index.sig /repo.pub')
            print('Fuse-X: octets, byte-identical creation, install/run/remove, network install, default/software signature verification and tamper rejection passed',flush=True)
        finally:
            guest.close()
            (ROOT/'build/pkg/guest.log').write_bytes(guest.log)

if __name__=='__main__':main()
