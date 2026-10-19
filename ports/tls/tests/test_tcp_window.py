#!/usr/bin/env python3
"""Run Unix-sized TCP rings against independent wire/checksum fixtures."""
from pathlib import Path
import subprocess,tempfile
from guest import Guest,ROOT
with tempfile.TemporaryDirectory(prefix='tcp-window-') as temporary:
    d=Path(temporary);exe=d/'tcp-window.fxe'
    objects=sorted((ROOT/'fuse-x/build/runtime/net').glob('*.ir'))
    assert objects
    command=['python3',str(ROOT/'fuse-x/cc.py'),str(ROOT/'ports/tls/tests/tcp_window.c'),'-o',str(exe)]
    for obj in objects:command+=['--object-ir',str(obj)]
    subprocess.run(command,check=True)
    g=Guest(d,'native',{'tcp-window':exe})
    try:
        text=g.shell('tcp-window; echo WINDOW_STATUS:$?',60)
        print(text,flush=True)
        assert 'large TCP window passed' in text and 'WINDOW_STATUS:0' in text,text
    finally:g.close()
