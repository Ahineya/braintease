#!/usr/bin/env python3
"""Run known-answer and host-entropy checks in an actual guest kernel."""
import argparse
from pathlib import Path
import tempfile
from guest import Guest, ROOT
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('native','ripple'),default='native')
p.add_argument('--executable',type=Path)
p.add_argument('--software',action='store_true')
a=p.parse_args()
exe=a.executable or ROOT/'build/tls'/('cryptocheck.fxe' if a.target=='native' else 'cryptocheck.rxe')
with tempfile.TemporaryDirectory(prefix='ripple-tls-crypto-') as directory:
    guest=Guest(directory,a.target,{'cryptocheck':exe})
    try:
        text=guest.shell(('RIPPLE_TLS_SOFTWARE=1 ' if a.software else '')+'RIPPLE_TLS_PROFILE=1 cryptocheck; echo CRYPTO_STATUS:$?',300)
        print(text,flush=True)
        assert 'cryptocheck passed' in text and 'CRYPTO_STATUS:0' in text,text
        assert ('TLS crypto accelerator: software' if a.software else 'TLS crypto accelerator: enabled') in text,text
    finally:guest.close()
print('Guest crypto known-answer and host-seeded DRBG checks passed')
