#!/usr/bin/env python3
"""Independent gzip/deflate stream fixtures, truncation and output bounds."""
import argparse,gzip,json,os,random,struct,subprocess,sys,tempfile,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--guest',action='store_true');p.add_argument('--asan',action='store_true',help='also enable AddressSanitizer (requires a compatible host runtime)');a=p.parse_args()
build=ROOT/'build/gzip-test';build.mkdir(exist_ok=True)
source=bytes((i*37+i//257)%256 for i in range(70000))
items=[]
for level in (0,1,9):items.append((gzip.compress(source,compresslevel=level,mtime=0),source))
# Long-distance history after ring wrap, using independent pseudo-random input.
rng=random.Random(123);prefix=rng.randbytes(32768);long=prefix+prefix+prefix[:1200]
items.append((gzip.compress(long,mtime=0),long))
head=b'\x1f\x8b\x08\x1e'+bytes(6)+b'\x04\0extra'[:6]+b'name\0comment\0'
head+=struct.pack('<H',zlib.crc32(head)&65535)
raw=zlib.compress(source,wbits=-15)
items.append((head+raw+struct.pack('<II',zlib.crc32(source),len(source)),source))
items.append((gzip.compress(source[:3000],mtime=0)+gzip.compress(source[3000:],mtime=0),source))
lines=['struct gzip_vector {const unsigned char *data;unsigned long size,length;uint32_t hash;};']
for i,(encoded,decoded) in enumerate(items):
 h=2166136261
 for b in decoded:h=((h^b)*16777619)&0xffffffff
 lines.append('static const unsigned char data%d[]={%s};'%(i,','.join(map(str,encoded))))
 items[i]=(encoded,decoded,h)
lines.append('static const struct gzip_vector gz_vectors[]={'+','.join('{data%d,sizeof(data%d),%d,0x%08xUL}'%(i,i,len(d),h) for i,(e,d,h) in enumerate(items))+'};')
# Trailer corruption, reserved flags, oversized optional fields, invalid DEFLATE,
# concatenation bounds and trailing garbage must all reject the whole entity.
base=items[2][0]
bad=[base[:-8]+bytes([base[-8]^1])+base[-7:],base[:-1]+bytes([base[-1]^1]),
     base[:3]+bytes([base[3]|0x20])+base[4:],base+b'junk',
     gzip.compress(b'x',mtime=0)*17,
     b'\x1f\x8b\x08\x08'+bytes(6)+b'x'*16385+b'\0',
     b'\x1f\x8b\x08\0'+bytes(6)+b'\x07'+bytes(8)]
crc_header=bytearray(items[4][0]);crc_header[len(head)-1]^=1;bad.append(bytes(crc_header))
for i,data in enumerate(bad):lines.append('static const unsigned char bad%d[]={%s};'%(i,','.join(map(str,data))))
lines.append('static const struct gzip_vector gz_invalid[]={'+','.join('{bad%d,sizeof(bad%d),1048576,0}'%(i,i) for i in range(len(bad)))+'};')
(build/'gzip-fixtures.h').write_text('\n'.join(lines)+'\n')
flags=['cc','-std=c99','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined,address' if a.asan else '-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(build)]
if sys.platform=='darwin':flags+=['-isysroot',os.environ.get('SDKROOT','/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')]
sources=[ROOT/'libraries/tests/gzip.c',ROOT/'libraries/deflate/deflate.c',ROOT/'libraries/deflate/gzip.c']
subprocess.run([*flags,*map(str,sources),'-o',str(build/'check')],check=True)
subprocess.run([str(build/'check')],check=True,timeout=120)
if a.guest:
 sys.path.insert(0,str(ROOT/'ports/tls/tests'));from guest import Guest
 subprocess.run(['python3',str(ROOT/'fuse-x/cc.py'),*map(str,sources),'-I'+str(build),'-o',str(build/'check.fxe')],check=True)
 with tempfile.TemporaryDirectory(prefix='gzip-guest-',dir='/tmp') as temporary:
  g=Guest(temporary,'native',{'gzipcheck':build/'check.fxe'},blocks=65536)
  try:
   text=g.shell('gzipcheck; echo GZIP_STATUS:$?',180);print(text,flush=True)
   assert 'failures=0' in text and 'GZIP_STATUS:0' in text,text
  finally:(build/'guest.log').write_bytes(g.log);g.close()
