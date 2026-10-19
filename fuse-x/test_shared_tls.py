#!/usr/bin/env python3
"""Verified HTTPS through dynamically linked guest libc/TLS, on a private NIC."""
import argparse
import re
from pathlib import Path
import shutil
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from fixtures import make
from guest import Guest
from peer import Services,BINARY


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'fuse-x/build/shared-test')
    args=p.parse_args();build=args.build.resolve()
    (build/'apps').mkdir(exist_ok=True)
    for name in ('sh','echo','cat','cmp','rm'):
        source=ROOT/f'fuse-x/build/apps/{name}.fxe'
        target=build/f'apps/{name}.fxe'
        if not target.exists():shutil.copyfile(source,target)
    with tempfile.TemporaryDirectory(prefix='shared-https-',dir='/tmp') as temporary:
        directory=Path(temporary);cert=make(directory/'cert');peer=Services(directory/'nic',cert)
        expected=directory/'expected';expected.write_bytes(BINARY)
        files=[dict(name='/etc/ssl/cert.pem',path=str(cert/'ca.pem')),
               dict(name='/expected',path=str(expected))]
        files += [dict(name='/page.html',text='<html><body>Shared library browser check</body></html>'),
                  dict(name='/etc/lynx.cfg',path=str(ROOT/'ports/lynx/fixtures/lynx.cfg'))]
        files += [dict(name='/lib/fuse-x/'+path.name,path=str(path)) for path in (build/'lib').glob('*.fxl.*') if re.fullmatch(r'.+\.fxl\.\d+',path.name)]
        guest=None
        try:
            def application(name):
                path=build/'apps'/f'{name}.fxe'
                return path if path.exists() else build/f'{name}.fxe'
            applications={'htget':application('htget'),'cmp':application('cmp')}
            if application('lynx').exists():applications['lynx']=application('lynx')
            guest=Guest(directory,'native',applications,files,peer,blocks=65536,build_override=build)
            for url in ('https://tls.test/binary','https://tls.test:444/binary','https://tls.test/chunked'):
                text=guest.shell(f'htget {url} /download; echo SHARED_HTTPS:$?',120)
                print(text,flush=True);assert 'SHARED_HTTPS:0' in text,text
                text=guest.shell('cmp /expected /download; echo SHARED_BODY:$?')
                assert 'SHARED_BODY:0' in text,text
            text=guest.shell('htget https://wrong.test/binary /download; echo SHARED_REJECT:$?',120)
            print(text,flush=True);assert 'SHARED_REJECT:1' in text,text
            assert 'SHARED_BODY:0' in guest.shell('cmp /expected /download; echo SHARED_BODY:$?')
            assert {'TLSv1.2','TLSv1.3'}<=set(peer.versions),peer.versions
            if 'lynx' in applications:
                text=guest.shell('lynx -dump /page.html; echo SHARED_LYNX:$?',120)
                print(text,flush=True)
                assert 'SHARED_LYNX:0' in text and 'Shared library browser check' in text,text
        finally:
            if guest:guest.close()
            peer.close()
    print('Shared libc/TLS: TLS 1.2/1.3, binary/chunked transfers, certificate rejection and preserved output passed')


if __name__=='__main__':main()
