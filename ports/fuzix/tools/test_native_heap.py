#!/usr/bin/env python3
"""Large sparse Fuse-X heap, legacy heap ABI, fork/exec and allocator edges."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def main():
    build=ROOT/'build/native-heap';build.mkdir(exist_ok=True)
    for name,source in [('heapbig','heapbigcheck.c'),('heapedge','heapedgecheck.c')]:
        subprocess.run(['python3',str(ROOT/'fuse-x/cc.py'),str(Path(__file__).with_name(source)),
            '-o',str(build/(name+'.fxe'))],check=True)
    with tempfile.TemporaryDirectory(prefix='native-heap-',dir='/tmp') as folder:
        guest=Guest(folder,'native',{name:build/(name+'.fxe') for name in ['heapbig','heapedge']},blocks=65536)
        try:
            for app,marker in [('heapbig','HEAP_BIG_OK'),('heapedge','HEAP_EDGES_OK')]:
                text=guest.shell(app+'; echo HEAP_STATUS:$?',180);print(text,flush=True)
                assert marker in text and 'HEAP_STATUS:0' in text,text
            # A new exec must remain usable after the exhaustion/near-limit test.
            assert 'HEAP_ALIVE' in guest.shell('echo HEAP_ALIVE')
        finally:
            (build/'guest.log').write_bytes(guest.log)
            try:guest.close()
            finally:(build/'guest.log').write_bytes(guest.log)
    # Compile a separate bounded kernel and SDK. Overlay the matching libc for
    # both the probe and the ordinary Bourne shell; don't mix installed libraries.
    bounded=build/'kernel-bounded';kernel=bounded/'fuse-x';sdk=build/'sdk-bounded'
    subprocess.run(['make','-C',str(ROOT/'fuse-x'),'BUILD='+str(bounded),
        'FUSE_X_HEAP_PROFILE=bounded','-j4',str(kernel)],check=True)
    for name,source in [('heapbound','heapboundcheck.c'),('heapedge','heapedgecheck.c')]:
        subprocess.run(['python3',str(ROOT/'fuse-x/cc.py'),str(Path(__file__).with_name(source)),
            '--heap-profile','bounded','--build',str(sdk),'-o',str(build/(name+'-bounded.fxe'))],check=True)
    with tempfile.TemporaryDirectory(prefix='bounded-heap-',dir='/tmp') as folder:
        prior=os.environ.get('FUSE_X_TEST_KERNEL');os.environ['FUSE_X_TEST_KERNEL']=str(kernel)
        try:
            guest=Guest(folder,'native',{name:build/(name+'-bounded.fxe') for name in ['heapbound','heapedge']},
                files=[dict(name='/lib/fuse-x/'+p.name,path=str(p)) for p in (sdk/'lib').glob('*.fxl.[0-9]')],blocks=65536)
        finally:
            if prior is None:os.environ.pop('FUSE_X_TEST_KERNEL',None)
            else:os.environ['FUSE_X_TEST_KERNEL']=prior
        try:
            for app,marker in [('heapbound','HEAP_BOUND_OK'),('heapedge','HEAP_EDGES_OK')]:
                text=guest.shell(app+'; echo HEAP_STATUS:$?',180);print(text,flush=True)
                assert marker in text and 'HEAP_STATUS:0' in text,text
        finally:
            (build/'bounded-guest.log').write_bytes(guest.log)
            try:guest.close()
            finally:(build/'bounded-guest.log').write_bytes(guest.log)
    print('Hosted sparse and bounded kernel/SDK heaps: large allocations, legacy ABI, COW, exec, copy boundaries and allocator edges passed')
if __name__=='__main__':main()
