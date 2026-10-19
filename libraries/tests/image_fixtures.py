"""Deterministic image vectors; PNG/GIF encoders are independent of guest code."""
from pathlib import Path
import struct,zlib,subprocess
ROOT=Path(__file__).resolve().parents[2]

def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data))
def fnv(data):
 h=2166136261
 for b in data:h=((h^b)*16777619)&0xffffffff
 return h

def png(w,h,depth,kind,interlace=False,trans=False,level=6):
 channels={0:1,2:3,3:1,4:2,6:4}[kind];maximum=(1<<depth)-1
 palette=bytes(v for i in range(1<<min(depth,8)) for v in ((i*17)&255,(i*43)&255,(i*79)&255)) if kind==3 else b''
 values=[];expected=bytearray()
 for y in range(h):
  row=[]
  for x in range(w):
   gray=(x*13+y*7)% (maximum+1)
   if kind in [0,4]:sample=[gray]+([((x+y)*23)%(maximum+1)] if kind==4 else [])
   elif kind==3:sample=[(x+y*3)%(1<<depth)]
   else:sample=[(x*11+y*3)%(maximum+1),(x*7+y*17)%(maximum+1),(x*19+y*13)%(maximum+1)]+([((x+y)*23)%(maximum+1)] if kind==6 else [])
   row.extend(sample)
   if kind==3:
    i=sample[0];rgba=list(palette[i*3:i*3+3])+[0 if trans and i==0 else 127 if trans and i==1 else 255]
   elif kind in [0,4]:
    val=gray>>8 if depth==16 else gray*255//maximum
    rgba=[val]*3+[sample[1]>>8 if depth==16 else sample[1]*255//maximum] if kind==4 else [val]*3+[0 if trans and gray==0 else 255]
   else:rgba=[c>>8 if depth==16 else c for c in sample[:3]]+[(sample[3]>>8 if depth==16 else sample[3]) if kind==6 else 0 if trans and sample[:3]==[0,0,0] else 255]
   expected.extend(rgba)
  values.append(row)
 passes=[(0,0,1,1)] if not interlace else [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
 raw=bytearray();bpp=max(1,(channels*depth+7)//8)
 for sx,sy,dx,dy in passes:
  if sx>=w or sy>=h:continue
  previous=None
  for y in range(sy,h,dy):
   samples=[v for x in range(sx,w,dx) for v in values[y][x*channels:(x+1)*channels]]
   if depth==16:row=bytes(v for s in samples for v in (s>>8,s&255))
   elif depth==8:row=bytes(samples)
   else:
    row=bytearray((len(samples)*depth+7)//8)
    for i,s in enumerate(samples):row[i*depth//8]|=s<<(8-depth-i*depth%8)
    row=bytes(row)
   filtered=bytearray();method=y%5
   for i,c in enumerate(row):
    a=row[i-bpp] if i>=bpp else 0;b=previous[i] if previous else 0;d=previous[i-bpp] if previous and i>=bpp else 0
    p=a+b-d;dist=[abs(p-v) for v in [a,b,d]];predict=[0,a,b,(a+b)//2,[a,b,d][dist.index(min(dist))]][method]
    filtered.append((c-predict)&255)
   raw.append(method);raw.extend(filtered);previous=row
 payload=zlib.compress(raw,level)
 data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',w,h,depth,kind,0,0,int(interlace)))
 if palette:data+=chunk(b'PLTE',palette)
 if trans:
  data+=chunk(b'tRNS',bytes([0,127]+[255]*(len(palette)//3-2)) if palette else b'\x00\x00'*(1 if kind==0 else 3))
 # Deliberately split the zlib stream across multiple IDAT boundaries.
 data+=b''.join(chunk(b'IDAT',payload[i:i+113]) for i in range(0,len(payload),113))+chunk(b'IEND',b'')
 return data,bytes(expected)

def gif(w,h,interlace=False,transparent=False,local=False):
 palette=bytes(v for i in range(16) for v in (i*17,(15-i)*17,(i*43)&255))
 rows=list(range(h)) if not interlace else [y for start,step in [(0,8),(4,8),(2,4),(1,2)] for y in range(start,h,step)]
 indices=[(x+y*3)%16 for y in rows for x in range(w)]
 codes=[16]+indices+[17];nextcode=18;bits=5;old=False;tag=have=0;compressed=bytearray()
 for code in codes:
  tag|=code<<have;have+=bits
  while have>=8:compressed.append(tag&255);tag>>=8;have-=8
  if code==16:nextcode=18;bits=5;old=False
  elif code!=17:
   if old and nextcode<4096:
    nextcode+=1
    if nextcode==1<<bits and bits<12:bits+=1
   old=True
 if have:compressed.append(tag&255)
 data=b'GIF89a'+struct.pack('<HHBBB',w,h,3 if local else 0x83,0,0)+(b'' if local else palette)
 if transparent:data+=b'!\xf9\x04\x01\x00\x00\x00\x00'
 data+=b','+struct.pack('<HHHHB',0,0,w,h,(0x83 if local else 0)|(64 if interlace else 0))+(palette if local else b'')+b'\x04'
 data+=b''.join(bytes([len(compressed[i:i+255])])+compressed[i:i+255] for i in range(0,len(compressed),255))+b'\x00;'
 expected=bytes(c for y in range(h) for x in range(w) for c in ((list(palette[((x+y*3)%16)*3:((x+y*3)%16)*3+3])+[255]) if not transparent or (x+y*3)%16 else [0,0,0,0]))
 return data,expected

def bmp(w,h,bpp=24,top=False):
 stride=((w*bpp+31)//32)*4;rows=[];expected=bytearray()
 for y in range(h):
  row=bytearray()
  for x in range(w):
   r,g,b=x*11%256,y*17%256,(x*7+y*13)%256
   expected.extend([r,g,b,255]);row.extend([b,g,r]+([0] if bpp==32 else []))
  row.extend(bytes(stride-len(row)));rows.append(row)
 raw=b''.join(rows if top else reversed(rows));offset=54
 return b'BM'+struct.pack('<IHHI',offset+len(raw),0,0,offset)+struct.pack('<IiiHHIIiiII',40,w,-h if top else h,1,bpp,0,len(raw),0,0,0,0)+raw,bytes(expected)

def indexed_bmp(w,h,bits,os2=False):
 count=1<<bits;palette=[(i*17%256,i*43%256,i*79%256) for i in range(count)]
 stride=((w*bits+31)//32)*4;rows=[];expected=bytearray()
 for y in range(h):
  row=bytearray(stride)
  for x in range(w):
   i=(x+y*3)%count;row[x*bits//8]|=i<<(8-bits-(x*bits)%8);expected.extend([*palette[i],255])
  rows.append(row)
 table=bytes(c for r,g,b in palette for c in ([b,g,r] if os2 else [b,g,r,0]))
 header=struct.pack('<IHHHH',12,w,h,1,bits) if os2 else struct.pack('<IiiHHIIiiII',40,w,h,1,bits,0,stride*h,0,0,0,0)
 offset=14+len(header)+len(table)
 return b'BM'+struct.pack('<IHHI',offset+stride*h,0,0,offset)+header+table+b''.join(reversed(rows)),bytes(expected)

def gif_offset(extra=False):
 # Codes [clear=4, red=1, dictionary=6, end=5]. Code 6 is the KwKwK case.
 head=b'GIF89a'+struct.pack('<HHBBB',5,3,0x80,0,0)+bytes([0,0,0,255,0,0])
 frame=b','+struct.pack('<HHHHB',1,1,3,1,0)+bytes([2,2,0x8c,0x0b,0])
 rgba=bytes(c for y in range(3) for x in range(5) for c in ([255,0,0,255] if y==1 and 1<=x<=3 else [0,0,0,255]))
 return head+frame+(frame if extra else b'')+b';',rgba

def make(folder):
 folder=Path(folder);folder.mkdir(parents=True,exist_ok=True);vectors=[]
 def add(name,data,rgba,w,h,status=0,jpeg=False):
  path=folder/name;path.write_bytes(data)
  vectors.append(dict(name=name,path=path,width=w,height=h,status=status,hash=fnv(rgba),rgba=rgba if jpeg else b'',opaque=all(rgba[i]==255 for i in range(3,len(rgba),4))))
 for kind in [0,2,3,4,6]:
  depths=[1,2,4,8,16] if kind==0 else [1,2,4,8] if kind==3 else [8,16]
  for depth in depths:
   for interlace in [False,True]:
    data,rgba=png(19,13,depth,kind,interlace,kind in [0,2,3]);add(f'png-{kind}-{depth}-{int(interlace)}.png',data,rgba,19,13)
 for level in [0,1,9]:
  data,rgba=png(29,31,8,6,level=level);add(f'png-deflate-{level}.png',data,rgba,29,31)
 # Exercise the contiguous RGB path without a colour key, including rows
 # shorter than a predictor's byte stride and all five PNG filters.
 for w,h in [(1,1),(1,7),(2,7),(128,128)]:
  data,rgba=png(w,h,8,2);add(f'png-rgb-{w}-{h}.png',data,rgba,w,h)
 data,rgba=png(257,255,8,6);add('bank.png',data,rgba,257,255)
 for interlace in [False,True]:
  for local in [False,True]:
   data,rgba=gif(23,19,interlace,True,local);add(f'gif-{int(interlace)}-{int(local)}.gif',data,rgba,23,19)
 data,rgba=gif(256,128);add('gif-full-dictionary.gif',data,rgba,256,128)
 for extra in [False,True]:
  data,rgba=gif_offset(extra);add(f'gif-offset-{int(extra)}.gif',data,rgba,5,3)
 for bits in [1,4,8]:
  for os2 in [False,True]:
   data,rgba=indexed_bmp(17,11,bits,os2);add(f'bmp-indexed-{bits}-{int(os2)}.bmp',data,rgba,17,11)
 data,rgba=png(1,1,8,6,True);add('tiny-adam7.png',data,rgba,1,1)
 for bpp in [24,32]:
  for top in [False,True]:
   data,rgba=bmp(17,11,bpp,top);add(f'bmp-{bpp}-{int(top)}.bmp',data,rgba,17,11)
 for grey,sample in [(True,'1x1'),(False,'1x1'),(False,'2x1'),(False,'1x2'),(False,'2x2')]:
  w,h=19,17
  raw=bytes(c for y in range(h) for x in range(w) for c in ([((x+y)*5)%256] if grey else [x*11%256,y*13%256,(x*7+y*5)%256]))
  ppm=(f'P{5 if grey else 6}\n{w} {h}\n255\n').encode()+raw
  source=folder/'jpeg-input.ppm';source.write_bytes(ppm)
  data=subprocess.check_output(['cjpeg','-quality','90','-sample',sample,str(source)])
  reference=subprocess.check_output(['djpeg','-nosmooth','-rgb'],input=data)
  # djpeg emits a simple P6 PPM. Header has exactly three whitespace lines.
  head,dimensions,maximum,reference=reference.split(b'\n',3)
  assert head==b'P6' and dimensions==f'{w} {h}'.encode() and maximum==b'255'
  rgba=bytes(v for i in range(0,len(reference),3) for v in [*reference[i:i+3],255])
  add(f'jpeg-{int(grey)}-{sample}.jpg',data,rgba,w,h,jpeg=True)
 source.unlink()
 return vectors

def header(folder):
 vectors=make(folder);lines=['/* Generated independent codec fixtures. */']
 def array(name,data):lines.append('static const unsigned char '+name+'[]={'+','.join(map(str,data))+'};')
 for i,v in enumerate(vectors):
  array('image_data_'+str(i),v['path'].read_bytes())
  if v['rgba']:array('image_expected_'+str(i),v['rgba'])
 lines.append('static const struct vector vectors[]={')
 for i,v in enumerate(vectors):
  lines.append('{"%s",image_data_%d,sizeof(image_data_%d),%d,%d,0x%08xUL,%s,%d},'%(v['name'],i,i,v['width'],v['height'],v['hash'],'image_expected_'+str(i) if v['rgba'] else 'NULL',v['opaque']))
 lines.append('};')
 path=Path(folder)/'fixtures.h';path.write_text('\n'.join(lines)+'\n');return path
if __name__=='__main__':
 import sys
 print(header(Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/image/fixtures'))
