"""Isolated headless GX windows in fresh, explicitly populated offline images."""
from pathlib import Path
import json,os,re,sys,tempfile,time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
def ppm(path):
    data=Path(path).read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
    if not m:return None
    w,h=map(int,m.groups());pixels=data[m.end():]
    return (w,h,pixels) if len(pixels)==w*h*3 else None
class GUI:
    def __init__(self,out,applications,files):
        self.out=Path(out);self.out.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(prefix='graphite-gui-');work=Path(self.temp.name)
        self.frames=work/'frames';self.mailbox=work/'event.json'
        env=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(self.frames),FUSE_X_TEST_EVENT=str(self.mailbox))
        try:self.g=Guest(work,'native',applications,[dict(name='/dev/gfx',mode=0o20600,device=0x400),dict(name='/data/home',directory=True,mode=0o40755),*files],blocks=65536)
        finally:os.environ.clear();os.environ.update(env)
        self.g.shell('HOME=/data/home; export HOME; unset LVGL_SCALE')
    def wait(self,check,seconds=60):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            self.g.pump();value=check()
            if value:return value
            assert self.g.process.poll() is None,self.g.log[-6000:].decode(errors='replace')
        raise AssertionError('GUI wait timed out:\n'+self.g.log[-6000:].decode(errors='replace'))
    def pause(self,seconds=.3):
        end=time.monotonic()+seconds
        while time.monotonic()<end:self.g.pump()
    def windows(self):
        try:return json.loads((self.frames/'windows.json').read_text())
        except (FileNotFoundError,json.JSONDecodeError):return []
    def window(self,title):return next((w for w in self.windows() if w['title']==title),None)
    def send(self,wid,**values):
        self.wait(lambda:not self.mailbox.exists());tmp=self.mailbox.with_suffix('.tmp')
        tmp.write_text(json.dumps(dict(window=wid,**values)));tmp.replace(self.mailbox)
        self.wait(lambda:not self.mailbox.exists() or (wid!=0 and not any(w['id']==wid for w in self.windows())))
        if self.mailbox.exists():self.mailbox.unlink()
    def key(self,wid,key):
        self.send(wid,input=dict(type='key',key=key,down=True));self.send(wid,input=dict(type='key',key=key,down=False))
    def ctrl(self,wid,key,shift=False):
        self.send(wid,input=dict(type='key',key=224,down=True))
        if shift:self.send(wid,input=dict(type='key',key=225,down=True))
        self.key(wid,key)
        if shift:self.send(wid,input=dict(type='key',key=225,down=False))
        self.send(wid,input=dict(type='key',key=224,down=False))
    def click(self,wid,x,y):
        self.send(wid,input=dict(type='button',button=0,down=True,x=x,y=y));self.send(wid,input=dict(type='button',button=0,down=False,x=x,y=y))
    def text(self,wid,text):
        for c in text:self.send(wid,input=dict(type='text',scalar=ord(c)))
    def logged(self,marker,at=0,seconds=60):return self.wait(lambda:marker.encode() in self.g.log[at:],seconds)
    def frame(self,wid,scale=1,width=800,height=560):
        def read():
            try:
                v=ppm(self.frames/f'{wid}.ppm');w=next((w for w in self.windows() if w['id']==wid),None)
                return v if w and (w['width'],w['height'],w['scale'])==(width,height,scale) and v and v[:2]==(width*scale,height*scale) else None
            except FileNotFoundError:return None
        self.wait(read);self.pause(.5);return self.wait(read)
    def snapshot(self,wid,name,**geometry):
        self.frame(wid,**geometry);target=self.out/(name+'.ppm');target.write_bytes((self.frames/f'{wid}.ppm').read_bytes());return target
    def close(self):
        try:self.g.close()
        finally:(self.out/'guest.log').write_bytes(self.g.log);self.temp.cleanup()
