#!/usr/bin/env python3
"""Install actual catalog apps/libraries over guest Ethernet on a private disk."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from peer import HTTPServices

def main():
    report=json.loads((ROOT/'build/pkg/catalog/fuse-x/catalog.json').read_text())
    packages=report['packages']
    with tempfile.TemporaryDirectory(prefix='pkg-catalog-guest-') as tmp:
        work=Path(tmp).resolve();index=work/'index.v1';sig=work/'index.sig';key=work/'key.pem'
        entries=['RIPINDEX1\n'];objects={}
        for p in packages:
            archive=Path(p['archive']);data=archive.read_bytes();h=hashlib.sha256(data).hexdigest()
            path='/objects/'+h[:24]+'.rpk';objects[path]=archive
            entries.append('\t'.join([p['name'],p['version'],p['target'],str(len(data)),h,path[1:],','.join(p['depends'])])+'\n')
        index.write_text(''.join(entries))
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(key)],check=True,capture_output=True)
        modulus=subprocess.check_output(['openssl','rsa','-in',str(key),'-noout','-modulus']).decode().strip().split('=')[1].lower()+'\n'
        subprocess.run(['openssl','dgst','-sha256','-sign',str(key),'-out',str(sig),str(index)],check=True)
        class Peer(HTTPServices):
            def http(self,stream):
                received=bytes(stream.received)
                if getattr(stream,'answered',False) or b'\r\n\r\n' not in received:return
                stream.answered=True
                path=received.split(b' ')[1].decode();stream.path=path;self.paths.append(path)
                allowed=dict(objects,**{'/index.v1':index,'/index.sig':sig})
                assert path in allowed,path
                body=allowed[path].read_bytes();stream.chunk=1024
                stream.queue(b'HTTP/1.0 200 OK\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
        peer=Peer(work/'nic')
        files=[dict(name='/etc/pkg/repo.conf',text='url=http://repo.test\n'),dict(name='/etc/pkg/repo.pub',text=modulus),
               dict(name='/sample.bas',text='PRINT "PACKAGED_BASIC_OK"\nEND\n'),
               dict(name='/sample.tcl',text='puts "PACKAGED_TCL_OK"\n')]
        guest=Guest(work,'native',{'pkg':ROOT/'build/pkg/pkg-engine.fxe'},files=files,blocks=131072,peer=peer)
        def run(command,marker=None):
            output=guest.shell(command,seconds=360)
            assert 'pkg:' not in output,output
            if marker:assert marker in output,output
            print(command,flush=True)
            return output
        try:
            run('pkg update',f"Verified repository index: {len(packages)} packages")
            run('pkg search netsurf','netsurf 0.')
            run('pkg --adopt-seed add echo','Downloading echo')
            run('/bin/echo PACKAGED_ABSOLUTE_BIN_OK','PACKAGED_ABSOLUTE_BIN_OK')
            run('pkg add seq','Downloading seq')
            run('/usr/bin/seq 1 3','1\n2\n3')
            run('pkg --adopt-seed add cat','Downloading cat')
            run('/usr/bin/cat /sample.bas','PACKAGED_BASIC_OK')
            run('pkg add basic','Downloading basic')
            run('/usr/bin/basic /sample.bas','PACKAGED_BASIC_OK')
            run('pkg add picol','Downloading picol')
            run('/usr/bin/tcl /sample.tcl','PACKAGED_TCL_OK')
            run('pkg --adopt-seed add pkg','Downloading pkg')
            run('/usr/bin/pkg list','basic 0.')
            # Repeating the install resolves the closure but downloads nothing.
            assert 'Downloading' not in run('/usr/bin/pkg add basic')
            run('/usr/bin/pkg --discard-previous clean')
            run('/bin/echo CATALOG_GUEST_OK','CATALOG_GUEST_OK')
            print('Fuse-X catalog: signed full index, migration, absolute /bin forwarding, isolated libc, BASIC/Tcl and package-manager self-update passed',flush=True)
        finally:
            guest.close();(ROOT/'build/pkg/catalog-guest.log').write_bytes(guest.log)
if __name__=='__main__':main()
