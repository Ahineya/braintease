#!/usr/bin/env python3
"""Guest image decoding/rendering: real NetSurf, Nano-X, HTTP and verified TLS."""
import argparse,json,os,re,struct,subprocess,sys,tempfile,threading,time
from pathlib import Path
from guests import ROOT,image,check_disk
sys.path.insert(0,str(ROOT/'libraries/tests'))
import image_fixtures as codec
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make as certificates
from peer import Services as TLSService
sys.path.insert(0,str(ROOT/'ports/fusede/tools'))
from build_theme import png,unpack565

BG=(32,64,96)
def pack(c):return (c[0]>>3)<<11|(c[1]>>2)<<5|(c[2]>>3)
def over(c,bg=BG):
 fg=pack(c);bg=pack(bg);a=c[3]
 return sum(((((fg>>shift)&mask)*a+((bg>>shift)&mask)*(255-a)+127)//255)<<shift for shift,mask in [(11,31),(5,63),(0,31)])
ALPHA=bytes([255,0,0,255,255,0,0,128,255,0,0,0,0,255,0,255,
             0,0,255,255,255,255,255,128,255,255,255,0,255,255,0,255])

def fixtures(folder):
 folder.mkdir(parents=True,exist_ok=True)
 raw=b'\0'+ALPHA[:16]+b'\0'+ALPHA[16:]
 data=b'\x89PNG\r\n\x1a\n'+codec.chunk(b'IHDR',struct.pack('!IIBBBBB',4,2,8,6,0,0,0))+codec.chunk(b'IDAT',codec.zlib.compress(raw))+codec.chunk(b'IEND',b'')
 (folder/'alpha.png').write_bytes(data)
 (folder/'broken.png').write_bytes(data[:-3]+b'bad')
 large=b'\x89PNG\r\n\x1a\n'+codec.chunk(b'IHDR',struct.pack('!IIBBBBB',512,512,8,6,0,0,0))+codec.chunk(b'IDAT',b'123456')+codec.chunk(b'IEND',b'')
 (folder/'large.png').write_bytes(large)
 g,rgba=codec.gif_offset();(folder/'sample.gif').write_bytes(g)
 b,rgba=codec.bmp(17,11);(folder/'sample.bmp').write_bytes(b)
 # An independent host JPEG encoder/reference; no Pillow dependency.
 ppm=b'P6\n19 17\n255\n'+bytes(c for y in range(17) for x in range(19) for c in [x*11,y*13,(x*7+y*5)%256])
 (folder/'input.ppm').write_bytes(ppm)
 j=subprocess.check_output(['cjpeg','-quality','90','-sample','2x2',str(folder/'input.ppm')]);(folder/'sample.jpg').write_bytes(j)
 expected=subprocess.check_output(['djpeg','-nosmooth','-rgb'],input=j).split(b'\n',3)[3]
 (folder/'index.html').write_text('''<!doctype html><title>Image checks</title><style>
 html,body {margin:0;padding:0;background:#204060;height:800px}
 img,div {position:absolute;margin:0;padding:0;border:0}
 </style>
 <img src="alpha.png" width="128" height="64" style="left:16px;top:16px">
 <img src="http://tls.test/sample.gif" style="left:160px;top:16px;width:100px;height:60px">
 <img src="sample.jpg" style="left:280px;top:16px;width:114px;height:102px">
 <img src="sample.bmp" style="left:420px;top:16px;width:102px;height:66px">
 <div style="left:16px;top:150px;width:170px;height:65px;background-image:url(alpha.png)"></div>
 <div style="left:210px;top:150px;width:80px;height:50px;overflow:hidden"><img src="alpha.png" style="left:-25px;top:-12px;width:128px;height:64px"></div>
 <img src="broken.png" alt="Broken" style="left:320px;top:150px;width:70px;height:32px">
 <img src="large.png" alt="Limited" style="left:410px;top:150px;width:70px;height:32px">
 <table style="position:absolute;left:320px;top:230px;border-collapse:collapse"><tr><td style="padding:0;border:0"><img src="alpha.png" width="128" height="64" style="position:static"></td></tr></table>
 <img src="https://tls.test/binary" style="left:16px;top:450px;width:128px;height:64px">
 ''')
 return data,g,expected

class Services(TLSService):
 def __init__(self,path,certs,alpha,gif):
  self.gif=gif;super().__init__(path,certs,alpha,b'image/png')
 def http(self,stream):
  if getattr(stream,'answered',False) or b'\r\n\r\n' not in stream.received:return
  stream.answered=True;request=bytes(stream.received);path=request.split(b' ')[1].decode();stream.path=path;self.paths.append(path)
  body=self.gif if path=='/sample.gif' else b''
  stream.queue(b'HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)

def verify(words,stride,ox,oy,jpeg,scroll=0):
 def check(x,y,wanted,tolerance=0):
  x+=ox;y+=oy-scroll
  if not(0<=y<480):return
  value=words[y*stride+x]
  if tolerance:
   assert all(abs(a-b)<=tolerance for a,b in zip(unpack565(value),unpack565(wanted))),(x,y,hex(value),hex(wanted))
  else:assert value==wanted,(x,y,hex(value),hex(wanted))
 if not scroll:
  for row in range(2):
   for col in range(4):check(16+col*32+16,16+row*32+16,over(ALPHA[(row*4+col)*4:(row*4+col+1)*4]))
  check(160+50,16+30,pack((255,0,0)));check(160+10,16+10,0)
  for x,y in [(1,1),(8,7),(16,10)]:
   pos=(y*19+x)*3;check(280+x*6+3,16+y*6+3,pack(jpeg[pos:pos+3]),16)
   check(420+x*6+3,16+y*6+3,pack((x*11,y*17,(x*7+y*13)%256)))
  # Tiny transparent CSS tiles retain phase across tile/clip boundaries.
  for x,y in [(16,150),(17,150),(18,150),(183,213),(145,181)]:
   pos=(((y-150)%2)*4+(x-16)%4)*4;check(x,y,over(ALPHA[pos:pos+4]))
  for x,y in [(211,151),(240,172),(288,198)]:
   sx=(x-210+25)*4//128;sy=(y-150+12)*2//64;pos=(sy*4+sx)*4
   check(x,y,over(ALPHA[pos:pos+4]))
  check(205,170,pack(BG));check(292,170,pack(BG))
  for col in range(4):check(320+col*32+16,230+16,over(ALPHA[col*4:col*4+4]))
 else:
  for col in range(4):check(16+col*32+16,450+16,over(ALPHA[col*4:col*4+4]))

def ppm(path):
 data=path.read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data);w,h=map(int,m.groups());rgb=data[m.end():]
 assert len(rgb)==w*h*3;return w,h,[pack(rgb[i:i+3]) for i in range(0,len(rgb),3)],rgb

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--target',choices=['native','ripple'],default='native')
 parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf');parser.add_argument('--rvm-test-binary',type=Path)
 args=parser.parse_args();args.build=args.build.resolve();native=args.target=='native';suffix='.fxe' if native else '.rxe'
 output=args.build/('images-'+args.target);output.mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='netsurf-images-') as temp:
  work=Path(temp);assets=work/'assets';alpha,gif,jpeg=fixtures(assets);certs=certificates(work/'certs');peer=Services(work/'nic',certs,alpha,gif)
  build=ROOT/('fuse-x/build' if native else 'build/fuzix')
  apps={n:args.build/(n+suffix) for n in ['netsurf','htget']};apps.update({n:build/('apps/'+n+suffix if native else n+suffix) for n in ['nano-X','sh','kill','echo']})
  files=[dict(name='/images/'+p.name,path=str(p)) for p in assets.iterdir() if p.suffix!='.ppm']
  files += [dict(name='/etc/ssl/cert.pem',path=str(certs/'ca.pem')),dict(name='/dev/gfx',mode=0o20600,device=0x400)]
  if native:
   apps['nxhostcheck']=build/'apps/nxhostcheck.fxe';old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(work/'frames'),NANOX_KEEP_TTY='1')
   try:g=Guest(work,'native',apps,files,peer,blocks=131072)
   finally:os.environ.clear();os.environ.update(old)
   try:
    g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('s=$!')
    if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
    # Defaults must avoid HTTP/HTTPS and local image fetches entirely.
    g.shell('netsurf --trace /images/index.html &');g.shell('p=$!')
    if b'NETSURF_GUI_LOADED Image checks' not in g.log:g.read(b'NETSURF_GUI_LOADED Image checks',300)
    assert not peer.paths and not peer.sni,(peer.paths,peer.sni)
    assert b'NETSURF_GUI_FILE /images/alpha.png' not in g.log
    wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',g.log)[-1]);g.shell(f'nxhostcheck ctl close {wid}')
    assert 'IMAGES_OFF_STATUS:0' in g.shell('wait $p; echo IMAGES_OFF_STATUS:$?')
    start=len(g.log)
    g.shell('netsurf --images --trace /images/index.html &');g.shell('p=$!')
    if b'NETSURF_GUI_LOADED Image checks' not in g.log[start:]:g.read(b'NETSURF_GUI_LOADED Image checks',300)
    deadline=time.monotonic()+20
    while True:
     g.pump()
     try:
      windows=json.loads((work/'frames/windows.json').read_text());item=next(w for w in windows if w['visible'] and w['title']=='NetSurf: Image checks')
      w,h,words,rgb=ppm(work/f"frames/{item['id']}.ppm");verify(words,w,0,64,jpeg);break
     except (FileNotFoundError,StopIteration,json.JSONDecodeError,AssertionError):
      if time.monotonic()>deadline:raise
    png(output/'native-images.png',w,h,[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
    wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',g.log)[-1]);g.shell(f'nxhostcheck ctl close {wid}')
    assert 'IMAGE_BROWSER_STATUS:0' in g.shell('wait $p; echo IMAGE_BROWSER_STATUS:$?')
    g.shell('kill $s; wait $s')
   finally:
    (output/'guest.log').write_bytes(g.log);g.close();peer.close()
  else:
   extra=[dict(name=f['name'],host=f['path']) if 'path' in f else f for f in files]
   extra += [dict(name='/dev/urandom',mode=0o20444,device=0x104),dict(name='/dev/crypto',mode=0o20666,device=0x105),dict(name='/etc/resolv.conf',data='nameserver 192.168.64.1\n')]
   disk=image('fuzix',build,apps,work,extra);stop=threading.Event();errors=[]
   def pump():
    try:
     while not stop.is_set():peer.pump();time.sleep(.001)
    except BaseException as error:errors.append(error)
   thread=threading.Thread(target=pump);thread.start()
   try:
    command=([str(args.rvm_test_binary.resolve()),'--ignored','--nocapture'] if args.rvm_test_binary else ['cargo','test','--release','-p','rvm','--test','netsurf','--','--ignored','--nocapture'])
    subprocess.run(command,cwd=ROOT,check=True,env=dict(os.environ,NETSURF_TEST_DISK=str(disk),NETSURF_TEST_OUTPUT=str(output),NETSURF_RVM_BUILD=str(build),NETSURF_TEST_NET=str(work/'nic'),NETSURF_TEST_IMAGES='1'))
   finally:stop.set();thread.join();peer.close()
   assert not errors,errors;check_disk(disk,False)
   log=(output/'rvm.log').read_text();scroll=int(re.search(r'NETSURF_GUI_SCROLL 0 (\d+)',log).group(1))
   for filename,offset in [('rvm-images.rgb565',0),('rvm-images-scroll.rgb565',scroll)]:
    source=output/filename;data=source.read_bytes();words=struct.unpack('<'+'H'*(len(data)//2),data);verify(words,640,24,100,jpeg,offset)
    png(source.with_suffix('.png'),640,480,[(*unpack565(p),255) for p in words])
  assert '/sample.gif' in peer.paths and '/binary' in peer.paths and 'tls.test' in peer.sni,peer.paths
  assert not peer.failures,peer.failures
 print(args.target+': PNG/GIF/JPEG/BMP, straight alpha, scaling, repetition, clipping, HTTP/HTTPS and clean shutdown passed')
if __name__=='__main__':main()
