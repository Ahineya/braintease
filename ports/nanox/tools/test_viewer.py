#!/usr/bin/env python3
"""Drive the image viewer through real hosted input on a disposable Fuse-X disk."""
import argparse,json,os,re,shutil,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'libraries/tests'))
from image_fixtures import png,bmp,gif

def fixtures(work,ripple=False):
    files=[]
    for name,data in [('colour.png',png(128,96,8,2)[0]),('alpha.png',png(128,96,8,6)[0]),
                      ('wide.bmp',bmp(512 if ripple else 1024,256)[0]),('palette.gif',gif(128,96)[0]),
                      ('photo.jpg',(ROOT/'ports/netsurf/fixtures/images/landscape.jpg').read_bytes()),
                      ('bad.png',b'not an image'),('huge.png',png(513,512,8,2)[0]),
                      ('oversize.bmp',b'BM'+bytes(1048576))]:
        path=work/name;path.write_bytes(data)
        files.append(dict(name='/pictures/'+name,path=str(path),mode=0o100644))
    return files

def rgb565(rgb):return (rgb[0]>>3)<<11|(rgb[1]>>2)<<5|(rgb[2]>>3)

def model():
    env=dict(os.environ)
    if sys.platform=='darwin' and 'SDKROOT' not in env:
        env['SDKROOT']=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
    with tempfile.TemporaryDirectory(prefix='nxview-model-') as folder:
        check=Path(folder)/'check'
        subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-fsanitize=undefined','-fno-sanitize-recover=all',str(ROOT/'ports/nanox/tools/view-model.c'),'-o',str(check)],check=True,env=env)
        subprocess.run([check],check=True)

def main():
    build=ROOT/'fuse-x/build';output=build/'nxview-tests';output.mkdir(exist_ok=True)
    apps={n:build/'apps'/f'{n}.fxe' for n in ('nano-X','nxview','nxselect','nxhostcheck','kill')}
    with tempfile.TemporaryDirectory(prefix='nxview-native-') as folder:
        work=Path(folder);frames=work/'frames';mailbox=work/'input.json'
        saved=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox),NANOX_KEEP_TTY='1',NANOX_TRACE='1')
        try:g=Guest(work,'native',apps,[dict(name='/dev/gfx',mode=0o20600,device=0x400),*fixtures(work)],blocks=131072)
        finally:os.environ.clear();os.environ.update(saved)
        def wait(test,seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                g.pump();value=test()
                if value:return value
                assert g.process.poll() is None,g.log[-5000:].decode(errors='replace')
            raise AssertionError('viewer wait timed out:\n'+g.log[-5000:].decode(errors='replace'))
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def window(title):return wait(lambda:next((w for w in registry() if w['visible'] and w['title']==title),None))
        def event(wid,**values):
            wait(lambda:not mailbox.exists());p=mailbox.with_suffix('.tmp');p.write_text(json.dumps(dict(window=wid,**values)));p.replace(mailbox)
            wait(lambda:not mailbox.exists())
        def key(wid,code,release=True):
            at=len(g.log);event(wid,input=dict(type='key',key=code,down=True))
            if release:event(wid,input=dict(type='key',key=code,down=False))
            return at
        def frame(wid,check=lambda w,h,p:True):
            def read():
                try:
                    data=(frames/f'{wid}.ppm').read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    if not m:return None
                    w,h=map(int,m.groups());p=data[m.end():]
                    if len(p)==w*h*3 and check(w,h,p):return w,h,p
                except FileNotFoundError:pass
            return wait(read)
        def capture(wid,name):
            frame(wid);shutil.copyfile(frames/f'{wid}.ppm',output/(name+'.ppm'))
        def pixels(wid,alpha=False):
            _,rgba=png(128,96,8,6 if alpha else 2)
            def check(w,h,p):
                left=(w-128)//2;top=28+(h-52-96)//2
                for y in range(0,96,7):
                    for x in range(0,128,11):
                        r,gc,b,a=rgba[(y*128+x)*4:(y*128+x)*4+4]
                        colour=rgb565((r,gc,b))
                        if alpha:
                            bg=rgb565((150,158,166) if (x//16+y//16)&1 else (112,120,128))
                            colour=sum((((colour>>shift)&mask)*a+((bg>>shift)&mask)*(255-a)+127)//255<<shift for shift,mask in [(11,31),(5,63),(0,31)])
                        at=((top+y)*w+left+x)*3
                        if rgb565(p[at:at+3])!=colour:return False
                return True
            return frame(wid,check)
        try:
            assert 'usage: nxview' in g.shell('nxview --help')
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('NANOX_TRACE=1; export NANOX_TRACE')
            g.shell('nano-X -p &');g.shell('server=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            names='colour.png alpha.png wide.bmp palette.gif photo.jpg bad.png huge.png oversize.bmp'
            g.shell('nxview '+' '.join('/pictures/'+n for n in names.split())+' &');g.shell('viewer=$!')
            wid=window('Image Viewer: colour.png')['id'];pixels(wid);capture(wid,'colour')
            key(wid,48);window('Image Viewer: alpha.png');pixels(wid,True);capture(wid,'alpha')
            # Native window size changes reflow fit geometry and clip image/chrome.
            for w,h in [(300,220),(1500,900),(32,32),(640,480)]:
                event(wid,resize=[w,h]);frame(wid,lambda ww,hh,p:ww==w and hh==h)
                if w>=300:pixels(wid,True)
            key(wid,48);window('Image Viewer: wide.bmp')
            at=key(wid,30);wait(lambda:b' 1024 256 0 0' in g.log[at:]) # actual size
            at=key(wid,79);wait(lambda:b' 1024 256 32 0' in g.log[at:])
            at=key(wid,46);wait(lambda:re.search(rb'NXVIEW_FRAME 640 480 1290 322 \d+ 0',g.log[at:]))
            event(wid,input=dict(type='button',button=0,down=True,x=250,y=150))
            at=len(g.log);event(wid,input=dict(type='motion',x=150,y=150));wait(lambda:b'NXVIEW_FRAME' in g.log[at:])
            event(wid,input=dict(type='button',button=0,down=False,x=150,y=150))
            capture(wid,'panned')
            for name in ('palette.gif','photo.jpg'):
                at=key(wid,48);window('Image Viewer: '+name);wait(lambda:b'NXVIEW_LOADED' in g.log[at:])
            capture(wid,'jpeg')
            # Three failures retain the previous decoded image and keep the viewer alive.
            at=key(wid,48);wait(lambda:b'Unrecognized image format' in g.log[at:])
            assert any(w['title']=='Image Viewer: photo.jpg' for w in registry())
            at=key(wid,48);wait(lambda:b'Image exceeds resource limits' in g.log[at:])
            at=key(wid,48);wait(lambda:b'Image file must be between' in g.log[at:])
            # Open dialog is asynchronous; Escape cancels and leaves the old picture.
            key(wid,18);selector=window('Select File')['id'];key(selector,41,False)
            wait(lambda:b'NXVIEW_PICKER_DONE' in g.log);assert window('Image Viewer: photo.jpg')
            at=len(g.log);key(wid,18);selector=window('Select File')['id']
            for x,y in [(20,50),(280,248)]:
                event(selector,input=dict(type='button',button=0,down=True,x=x,y=y))
                if y==50:event(selector,input=dict(type='button',button=0,down=False,x=x,y=y))
            wait(lambda:b'NXVIEW_LOADED /pictures/alpha.png' in g.log[at:]);pixels(wid,True)
            event(wid,close=True);assert 'VIEW_STATUS:0' in g.shell('wait $viewer; echo VIEW_STATUS:$?')
            # Explicit corrupt/oversized initial paths still produce a usable window.
            for name,error in [('huge.png',b'Image exceeds resource limits'),('oversize.bmp',b'Image file must be between')]:
                at=len(g.log);g.shell('nxview /pictures/'+name+' &');g.shell('viewer=$!')
                wid=window('Image Viewer')['id'];wait(lambda:error in g.log[at:]);event(wid,close=True)
                assert 'VIEW_STATUS:0' in g.shell('wait $viewer; echo VIEW_STATUS:$?')
            g.shell('kill $server; wait $server')
            assert not any(e in g.log for e in (b'GsError',b'native runtime error',b'panic:',b'Drawing failed'))
        finally:
            g.close();(output/'native.log').write_bytes(g.log)
    print('Image viewer/Fuse-X: PNG/alpha pixels, JPEG/GIF/BMP, fit, zoom, pan, tiny/full-HD resize, cancel, error preservation and cleanup passed')
def ripple(test_binary=None):
    build=ROOT/'build/fuzix';output=build/'nxview-tests';output.mkdir(exist_ok=True)
    for name,kind in [('colour',2),('alpha',6)]:(output/(name+'.rgba')).write_bytes(png(128,96,8,kind)[1])
    with tempfile.TemporaryDirectory(prefix='nxview-rvm-') as folder:
        work=Path(folder);files=fixtures(work,True)
        for name in ('sh','echo','cat','kill','nano-X','nanowm','nxdsktop','nxview','nxselect'):
            files.append(dict(name='/bin/'+name,path=str(build/(name+'.rxe')),mode=0o100755))
        manifest=work/'files.json';manifest.write_text(json.dumps(files));disk=work/'disk.img'
        builder='''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {Image,writeImage} from './ports/fuzix/tools/image.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,build,output]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest)).map(f=>({...f,data:fs.readFileSync(f.path)}));
const {fuseDEFiles}=await import('./ports/fusede/files.mjs');files.push(...fuseDEFiles(build+'/fusede'));
files.push({name:'/init',mode:0o100755,data:fs.readFileSync(build+'/init.rxe')},
{name:'/dev/gfx',mode:0o20600,device:0x400},{name:'/dev/tty',mode:0o20666,device:0x200},
{name:'/dev/console',mode:0o20600,device:0x201},{name:'/dev/null',mode:0o20666,device:0x100});
writeImage(output,fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},
{name:'ROOTFS.IMG',data:rootImage(files,32768,256)}],
{totalSectors:393216,sectorsPerFat:256,sectorsPerCluster:8,imageFactory:size=>new Image(size)}));
'''
        subprocess.run(['node','--input-type=module','-e',builder,str(manifest),str(build),str(disk)],cwd=ROOT,check=True)
        command=[str(Path(test_binary).resolve()),'--ignored','--nocapture'] if test_binary else ['cargo','test','--release','--jobs','2','-p','rvm','--test','image_viewer','--','--ignored','--nocapture']
        subprocess.run(command,cwd=ROOT,check=True,env=dict(os.environ,NXVIEW_TEST_BUILD=str(build),NXVIEW_TEST_OUTPUT=str(output),NXVIEW_TEST_DISK=str(disk)))
        subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(disk)],check=True)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['native','ripple','both'],default='both');p.add_argument('--rvm-test-binary');a=p.parse_args()
    model()
    if a.target in ('native','both'):main()
    if a.target in ('ripple','both'):ripple(a.rvm_test_binary)
