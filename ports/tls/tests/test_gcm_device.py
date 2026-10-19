#!/usr/bin/env python3
"""Exercise bounded GCM requests through the real guest ioctl and device."""
import argparse,tempfile
from pathlib import Path
from guest import Guest
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=['native','ripple'],default='native')
p.add_argument('--executable',type=Path,required=True)
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='gcm-device-',dir='/tmp') as temporary:
    g=Guest(temporary,a.target,{'gcmdevcheck':a.executable})
    try:
        text=g.shell('gcmdevcheck; echo GCM_STATUS:$?',300)
        print(text,flush=True)
        assert 'gcmdevcheck passed' in text and 'GCM_STATUS:0' in text,text
    finally:g.close()
print(a.target+': AES-GCM known answer, bank crossings, rejected-tag atomicity, malformed requests, maximum record and fork/COW passed')
