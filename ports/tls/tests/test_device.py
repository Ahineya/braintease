#!/usr/bin/env python3
"""Exercise the crypto ioctl across banks, invalid pointers and fork/COW."""
import argparse
import tempfile
from guest import Guest, ROOT
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('native','ripple'),default='native')
a=p.parse_args()
suffix='.fxe' if a.target=='native' else '.rxe'
with tempfile.TemporaryDirectory(prefix='ripple-cryptodev-') as directory:
    guest=Guest(directory,a.target,{'cryptodevcheck':ROOT/'build/tls-accel'/('cryptodevcheck'+suffix)})
    try:
        text=guest.shell('cryptodevcheck; echo DEVICE_STATUS:$?',180)
        print(text,flush=True)
        assert 'cryptodevcheck passed' in text and 'DEVICE_STATUS:0' in text,text
    finally:guest.close()
print('Crypto ioctl: bank boundaries, guards, invalid requests/pointers and fork/COW passed')
