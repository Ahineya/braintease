#!/usr/bin/env python3
"""Check actual gterm/nxterm Unicode pixels on disposable guest disks."""
import argparse,json,os,re,struct,subprocess,sys,tempfile,time,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'rfl/tools'))
from test_unicode import image,build_probe
from test_os import Guest,run,PORT

def font():
    source=(ROOT/'libraries/unicode/font.inc').read_text()
    codes,rows=source.split('static const uint16_t ru_font_rows[][16] = {')
    codes=[int(x,16) for x in re.findall(r'0x([0-9a-f]+)',codes)]
    rows=[[int(x,16) for x in re.findall(r'0x([0-9a-f]+)',line)] for line in rows.splitlines() if line.strip().startswith('{')]
    return dict(zip(codes,rows))

def png(path,width,height,rgb):
    def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data))
    raw=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

def check(width,height,rgb,output,origin=(0,0)):
    glyphs=font();fg=(173,170,173)
    ox,oy=origin
    for column,rows in [(1,glyphs[0x416]),(2,glyphs[0x393]),(3,[a|b for a,b in zip(glyphs[ord('e')],glyphs[0x301])])]:
        for y,bits in enumerate(rows):
            for x in range(8):
                at=((oy+y)*width+ox+column*8+x)*3
                expected=fg if bits&(0x8000>>x) else (0,0,0)
                assert tuple(rgb[at:at+3])==expected,(output,column,x,y,expected,tuple(rgb[at:at+3]))
    png(output,width,height,rgb)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['fuzix','fuse-x','both'],default='both')
    p.add_argument('--output',type=Path,default=ROOT/'build/unicode/render')
    p.add_argument('--skip-build',action='store_true')
    a=p.parse_args();output=a.output.resolve();output.mkdir(parents=True,exist_ok=True)
    for system in (['fuzix','fuse-x'] if a.os=='both' else [a.os]):
        native=system=='fuse-x';build=ROOT/('fuse-x/build' if native else 'build/fuzix');sdk=ROOT/'build/unicode'/('native' if native else 'fuzix')
        artifact=output/('utf8-output'+('.fxe' if native else '.rxe'))
        if not a.skip_build:
            if native:run(['python3',ROOT/'fuse-x/apps.py','sh','--build',sdk])
            else:run(['make','-s','-C',PORT,'-j4','BUILD='+str(sdk),'RFL_UNICODE=1','rfl-sdk',sdk/'sh.rxe'])
            build_probe(sdk,native,artifact,ROOT/'ports/graphics/tests/unicode-output.c')
            if native:run(['python3',ROOT/'fuse-x/graphics.py','--build',sdk,'--only','gterm','nxterm','nano-X'])
            else:run(['make','-s','-C',PORT,'-j8','BUILD='+str(sdk),*[sdk/(name+'.rxe') for name in ['gterm','nxterm','nano-X']]])
        apps={name:sdk/('apps/'+name+'.fxe' if native else name+'.rxe') for name in ['gterm','nxterm','nano-X','sh']}
        apps.update({'utf8-output':artifact,'echo':build/('apps/echo.fxe' if native else 'echo.rxe')})
        with tempfile.TemporaryDirectory(prefix='unicode-render-') as t:
            folder=Path(t);disk=image(system,build,apps,folder)
            if not native:
                raw=output/'gterm-fuzix.rgb565'
                run([ROOT/'target/release/rvm',build/'bios.bin','--disk',disk,'--memory','16384000','--jit','--net','off','--headless','--input','gterm /bin/utf8-output\n','--raw-frame',raw,'--capture-frame','3','--timeout','120'])
                pixels=struct.unpack('<'+'H'*(640*480),raw.read_bytes());rgb=bytearray()
                for c in pixels:
                    r,g,b=(c>>11)&31,(c>>5)&63,c&31;rgb.extend(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)))
                check(640,480,rgb,output/'gterm-fuzix.png')
            else:
                frame=folder/'frame.ppm';old=os.environ.get('FUSE_X_FRAME');os.environ['FUSE_X_FRAME']=str(frame)
                guest=Guest([build/'fuse-x','--run',disk],system)
                if old is None:os.environ.pop('FUSE_X_FRAME',None)
                else:os.environ['FUSE_X_FRAME']=old
                try:
                    guest.raw(guest.prompt)
                    for name in ['gterm','nxterm']:
                        if name=='nxterm':
                            guest.shell('nano-X -p &');guest.wait(lambda:b'Nano-X ready' in guest.log,'Nano-X service ready')
                        guest.send((name+' /bin/utf8-output\r').encode())
                        deadline=time.monotonic()+90;matched=False
                        # Capture while the child is alive; closing nxterm removes
                        # its window from the server's framebuffer.
                        while time.monotonic()<deadline and not matched:
                            if frame.exists():
                                data=frame.read_bytes();header=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                                if header:
                                    width,height=map(int,header.groups());rgb=data[header.end():]
                                    # Fuse-X can host each Nano-X client in its
                                    # own native window, whose pixels start at 0.
                                    origins=[(0,0)]+([] if name=='gterm' else [(24+i*16,32+i*16) for i in range(4)])
                                    for origin in origins:
                                        try:check(width,height,rgb,output/(name+'-fuse-x.png'),origin)
                                        except AssertionError:continue
                                        matched=True;break
                            if not matched:guest.pump()
                        assert matched,(name,'Unicode glyphs never appeared')
                        guest.raw(guest.prompt,seconds=90)
                    guest.send(b'shutdown\r');guest.process.wait(timeout=30);assert guest.process.returncode==0
                finally:(output/'native.log').write_bytes(guest.log);guest.close()
        print(system+': real terminal glyph pixels match Cyrillic, Greek and combining-accent bitmaps',flush=True)
if __name__=='__main__':main()
