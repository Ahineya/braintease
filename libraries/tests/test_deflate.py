#!/usr/bin/env python3
"""Independent zlib streams covering lazy bit reads across block boundaries."""
from pathlib import Path
import argparse
import random
import subprocess
import os
import sys
import zlib
ROOT=Path(__file__).resolve().parents[2]

def fixtures(folder):
    rng=random.Random(16);vectors=[]
    for raw in (b'',bytes(range(256))*3,b'ab'*33000,bytes(rng.randrange(256) for _ in range(1000)),b'abc123xyz'*400):
        for level in (0,1,6,9):
            for strategy in (zlib.Z_DEFAULT_STRATEGY,zlib.Z_FIXED):
                c=zlib.compressobj(level,zlib.DEFLATED,-15,strategy=strategy)
                vectors.append((c.compress(raw)+c.flush(),raw))
    # Non-final Huffman blocks followed by empty stored sync-flush blocks and
    # more literals. A lookahead optimization must not consume the next block.
    c=zlib.compressobj(6,zlib.DEFLATED,-15);parts=[];plain=b''
    for raw in (b'hello'*43,bytes(range(256)),b'Q'*777):
        parts.append(c.compress(raw)+c.flush(zlib.Z_SYNC_FLUSH));plain+=raw
    parts.append(c.flush());vectors.append((b''.join(parts),plain))
    lines=[]
    for i,(data,raw) in enumerate(vectors):
        assert zlib.decompress(data,-15)==raw
        for prefix,values in (('data',data),('plain',raw)):
            lines.append('static const unsigned char '+prefix+str(i)+'[]={'+','.join(map(str,values or b'\0'))+'};')
    lines.append('static const struct stream streams[]={')
    lines.extend('{data%d,plain%d,%dUL,%dUL},'%(i,i,len(data),len(raw)) for i,(data,raw) in enumerate(vectors))
    lines.append('};');folder.mkdir(parents=True,exist_ok=True)
    (folder/'deflate-fixtures.h').write_text('\n'.join(lines)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,default=ROOT/'build/deflate-test')
    args=p.parse_args();build=args.build.resolve();fixtures(build)
    program=build/'deflatecheck'
    flags=[]
    if sys.platform=='darwin':
        flags=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
    subprocess.run(['cc','-std=c99','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',
        *flags,'-I'+str(build),str(ROOT/'libraries/tests/deflate.c'),str(ROOT/'libraries/deflate/deflate.c'),'-o',str(program)],check=True)
    subprocess.run([str(program)],check=True,timeout=30)
if __name__=='__main__':main()
