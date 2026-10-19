#!/usr/bin/env python3
"""Real input/pixels/physics on disposable Fuse-X and RVM/JIT disks."""
import argparse,json,os,re,shutil,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/kolf'
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def fixtures(work,maximum=False):
    # Nine trivial greens isolate progression from the physics-specific tests.
    # Native releases a short backswing; RVM holds to the automatic maximum
    # (independent of renderer latency), whose fixture sinks after a wall bounce.
    text='[0-course@-50,-50]\nName=Acceptance greens\nauthor=Test\n'
    for i in range(1,10):
        cup=122 if maximum else 82
        text+=f'[{i}-ball@100,200]\ndummykey=true\n[{i}-cup@{cup},200|1]\ndummykey=true\n[{i}-hole@-50,-50|0]\npar=2\nmaxstrokes=10\nborderWalls=true\n'
    p=work/'straight.kolf';p.write_text(text)
    # Stable green for the unlimited sentinel and genuine-limit UI regressions.
    for name,limit in [('unlimited',4),('limited',1)]:
        (work/(name+'.kolf')).write_text('[1-ball@80,320]\ndummykey=true\n[1-cup@350,70|1]\ndummykey=true\n[1-hole@-50,-50|0]\npar=2\nmaxstrokes='+str(limit)+'\nborderWalls=true\n')
    (work/'water.kolf').write_text('[1-ball@80,200]\ndummykey=true\n[1-cup@350,70|1]\ndummykey=true\n[1-hole@-50,-50|0]\npar=2\nmaxstrokes=4\nborderWalls=true\n[1-puddle@120,200|2]\nwidth=40\nheight=60\n[1-floater@200,80|3]\nstartPoint=200,80\nendPoint=240,80\nwidth=40\nheight=24\nspeed=7\n')
    (work/'edge.kolf').write_text('[1-ball@4,200]\ndummykey=true\n[1-cup@350,350|1]\ndummykey=true\n[1-hole@-50,-50|0]\npar=2\nmaxstrokes=4\nborderWalls=true\n')
    (work/'advanced.kolf').write_text("""[0-course@-50,-50]
Name=Obstacle Laboratory
[1-ball@80,320]
dummykey=true
[1-cup@350,70|1]
dummykey=true
[1-hole@-50,-50|0]
par=4
maxstrokes=4
borderWalls=true
[1-puddle@220,200|2]
width=150
height=100
[1-bridge@180,170|3]
width=90
height=60
topWallVisible=true
botWallVisible=true
[1-floater@40,80|4]
startPoint=40,80
endPoint=120,80
width=80
height=35
speed=7
[1-cup@60,95|5]
dummykey=true
[1-windmill@260,150|6]
width=100
height=60
speed=6
leftWallVisible=true
rightWallVisible=true
bottom=true
[1-bumper@130,300|7]
dummykey=true
[1-blackhole@320,300|8]
exit=80,260
exitDeg=90
minspeed=1.78
maxspeed=3.75
[1-sand@100,120|9]
width=70
height=30
changeEnabled=true
changeEvery=0
[1-sign@20,200|10]
width=120
height=45
Comment=<strong>Mind the gate!</strong>
[1-slope@10,10|11]
width=150
height=45
gradient=Horizontal
grade=3
stuckOnGround=true
""")
    return [*[dict(name='/courses/'+name+'.kolf',path=str(work/(name+'.kolf'))) for name in ('straight','unlimited','limited','advanced','water','edge')],
            *[dict(name='/courses/'+p.name,path=str(p)) for p in sorted((PORT/'courses').glob('*.kolf'))],
            dict(name='/dev/gfx',mode=0o20600,device=0x400)]

def native(output):
    build=ROOT/'fuse-x/build';output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='kolf-native-') as folder:
        work=Path(folder);frames=work/'frames';mailbox=work/'input.json'
        env=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{n:build/'apps'/f'{n}.fxe' for n in ('nano-X','kolf','kolfcheck','kill')},fixtures(work),blocks=65536)
        finally:os.environ.clear();os.environ.update(env)
        def wait(test,seconds=60):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                g.pump();v=test()
                if v:return v
                assert g.process.poll() is None,g.log[-6000:].decode(errors='replace')
            raise AssertionError(g.log[-6000:].decode(errors='replace'))
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def window():return wait(lambda:next((w for w in registry() if w['visible'] and w['title'].startswith('Kolf /')),None))
        def event(wid,values):
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp');tmp.write_text(json.dumps(dict(window=wid,input=values)));tmp.replace(mailbox);wait(lambda:not mailbox.exists())
        def key(wid,key):
            event(wid,dict(type='key',key=key,down=True));event(wid,dict(type='key',key=key,down=False))
        def stroke(wid):
            at=len(g.log);event(wid,dict(type='key',key=44,down=True));wait(lambda:b'KOLF_SWING 0 1 ' in g.log[at:])
            time.sleep(.16);event(wid,dict(type='key',key=44,down=False));return at
        def capture(wid,name):
            file=frames/f'{wid}.ppm';wait(lambda:file.exists() and file.stat().st_size>610*430*3)
            time.sleep(.15);shutil.copyfile(file,output/name);return file.read_bytes()
        result={}
        try:
            result['physics']=g.shell('kolfcheck '+ ' '.join('/courses/'+p.name for p in sorted((PORT/'courses').glob('*.kolf'))));assert 'failures=0' in result['physics']
            assert 'holes=9' in g.shell('kolf --check-course')
            for name in ('Practice','ReallyEasy','Easy','Medium','Hard','Impossible','Classic','USApro'):
                assert 'KOLF_COURSE_OK' in g.shell('kolf --check-course '+name)
            result['benchmark']=g.shell('kolf --benchmark');assert 'steps=9000' in result['benchmark']
            g.shell('KOLF_TRACE=1; export KOLF_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');wait(lambda:b'Nano-X ready' in g.log)
            start=time.monotonic();at=len(g.log);g.shell('kolf &');g.shell('game=$!')
            wait(lambda:b'KOLF_READY' in g.log[at:]);w=window();wid=w['id'];result['practice_start_seconds']=time.monotonic()-start
            initial=capture(wid,'practice.ppm')
            key(wid,58);time.sleep(.25);modal=capture(wid,'help.ppm');assert modal!=initial
            key(wid,41);time.sleep(.25);assert capture(wid,'returned.ppm')==initial
            key(wid,6);time.sleep(.25);picker=capture(wid,'courses.ppm');assert picker!=initial
            key(wid,81);at=len(g.log);key(wid,40);wait(lambda:b'KOLF_HOLE 1 ' in g.log[at:])
            assert 'Really Easy' in window()['title'];capture(wid,'really-easy.ppm')
            key(wid,6);key(wid,82);at=len(g.log);key(wid,40);wait(lambda:b'KOLF_HOLE 1 ' in g.log[at:])
            assert 'Slope Practice' in window()['title']
            # Hold shows a real backswing; Escape cancels without a stroke,
            # including a later release of the key that started the swing.
            at=len(g.log);event(wid,dict(type='key',key=44,down=True));wait(lambda:b'KOLF_SWING 0 1 ' in g.log[at:])
            time.sleep(.15);backswing=capture(wid,'backswing.ppm');assert backswing!=initial
            key(wid,41);event(wid,dict(type='key',key=44,down=False));time.sleep(.15);g.pump();assert b'KOLF_SHOT' not in g.log[at:]
            # Mouse aims towards the pointer; distance does not set power.
            event(wid,dict(type='motion',x=45,y=290));at=len(g.log)
            event(wid,dict(type='button',button=0,down=True,x=45,y=290));wait(lambda:b'KOLF_SWING 0 1 ' in g.log[at:])
            time.sleep(.16);event(wid,dict(type='button',button=0,down=False,x=45,y=290));wait(lambda:b'KOLF_SHOT' in g.log[at:])
            key(wid,19);time.sleep(.25);paused=capture(wid,'paused.ppm');time.sleep(.4);assert capture(wid,'paused-again.ppm')==paused
            key(wid,21);time.sleep(.2)
            # Three discrete clicks: begin, select strength, select accuracy.
            key(wid,4);at=len(g.log);key(wid,44);wait(lambda:b'KOLF_SWING 1 1 ' in g.log[at:])
            time.sleep(.25);capture(wid,'circular-power.ppm');g.pump();assert b'KOLF_SHOT' not in g.log[at:]
            at=len(g.log);key(wid,44);wait(lambda:b'KOLF_SWING 1 2 ' in g.log[at:]);capture(wid,'circular-accuracy.ppm')
            at=len(g.log);key(wid,44);wait(lambda:b'KOLF_SHOT' in g.log[at:])
            key(wid,21);key(wid,4);at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            # A ball against the left wall can be aimed into the green without
            # needing room for the cursor behind it.
            at=len(g.log);g.shell('kolf /courses/edge.kolf &');g.shell('game=$!');wait(lambda:b'KOLF_READY' in g.log[at:]);wid=window()['id']
            event(wid,dict(type='motion',x=150,y=224));at=stroke(wid)
            wait(lambda:b'KOLF_SHOT' in g.log[at:]);wait(lambda:b'aim=256,0' in g.log[at:]);capture(wid,'edge-aim.ppm')
            at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            at=len(g.log);g.shell('kolf /courses/water.kolf &');g.shell('game=$!');wait(lambda:b'KOLF_READY' in g.log[at:]);wid=window()['id']
            for choice in (0,1,2):
                event(wid,dict(type='motion',x=230,y=224));at=stroke(wid)
                wait(lambda:b'KOLF_STATE 1 5 2 ' in g.log[at:]);wait(lambda:b'KOLF_PAINTED 1 0 5' in g.log[at:])
                modal=capture(wid,'water-choice.ppm');time.sleep(.3);assert capture(wid,'water-frozen.ppm')==modal
                at=len(g.log)
                if choice==2:
                    key(wid,24);wait(lambda:b'KOLF_UNDO 1 0 80000 200000' in g.log[at:]);continue
                if choice==0:key(wid,40) # Default is drop outside the hazard.
                else:event(wid,dict(type='button',button=0,down=True,x=200,y=245));event(wid,dict(type='button',button=0,down=False,x=200,y=245))
                wait(lambda:f'KOLF_RECOVER {choice} 2 '.encode() in g.log[at:]);capture(wid,'water-'+('drop' if choice==0 else 'rehit')+'.ppm')
                if choice==1:wait(lambda:b'KOLF_RECOVER 1 2 80000 200000' in g.log[at:])
                at=len(g.log)
                if choice==0:key(wid,24)
                else:
                    event(wid,dict(type='key',key=224,down=True));key(wid,29);event(wid,dict(type='key',key=224,down=False))
                wait(lambda:b'KOLF_UNDO 1 0 80000 200000' in g.log[at:])
            at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            # Real UI must keep a maxstrokes=4 green playable beyond four shots.
            for name,count,state in [('unlimited',5,0),('limited',1,3)]:
                at=len(g.log);g.shell('kolf /courses/'+name+'.kolf &');g.shell('game=$!');wait(lambda:b'KOLF_READY' in g.log[at:]);wid=window()['id']
                for shot in range(1,count+1):
                    at=len(g.log);key(wid,44);wait(lambda:f'KOLF_STATE 1 {state} {shot} '.encode() in g.log[at:])
                capture(wid,name+'.ppm');at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            at=len(g.log);g.shell('kolf /courses/advanced.kolf &');g.shell('game=$!');wait(lambda:b'KOLF_READY' in g.log[at:]);wid=window()['id']
            a=capture(wid,'advanced.ppm');time.sleep(.3);b=capture(wid,'advanced-moving.ppm');assert a!=b
            key(wid,19);time.sleep(.3);a=capture(wid,'advanced-paused.ppm');time.sleep(.3);assert capture(wid,'advanced-paused-again.ppm')==a
            at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            # Full round, including last-hole dialog and restart.
            at=len(g.log);g.shell('kolf /courses/straight.kolf &');g.shell('game=$!');wait(lambda:b'KOLF_READY' in g.log[at:]);wid=window()['id']
            for i in range(1,10):
                at=stroke(wid);wait(lambda:f'KOLF_STATE {i} 2 1 '.encode() in g.log[at:])
                if i==1:
                    at=len(g.log);key(wid,24);wait(lambda:b'KOLF_UNDO 1 0 100000 200000' in g.log[at:])
                    at=stroke(wid);wait(lambda:b'KOLF_STATE 1 2 1 ' in g.log[at:])
                if i<9:
                    at=len(g.log);key(wid,40);wait(lambda:f'KOLF_HOLE {i+1} '.encode() in g.log[at:])
            capture(wid,'round-complete.ppm');at=len(g.log);key(wid,40);wait(lambda:b'KOLF_HOLE 1 ' in g.log[at:])
            time.sleep(.25);stable=capture(wid,'idle.ppm');time.sleep(.5);assert capture(wid,'idle-again.ppm')==stable
            at=len(g.log);event(wid,dict(type='close'));wait(lambda:b'KOLF_EXIT' in g.log[at:]);g.shell('wait $game')
            g.shell('kill $server');g.shell('wait $server')
            for bad in (b'GsError',b'NXR_BUFFER_FAILED',b'panic:',b'FAIL line'):assert bad not in g.log
            result['status']='passed'
        finally:
            (output/'guest.log').write_bytes(g.log);g.close()
        (output/'report.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

def ripple(output,test_binary=None):
    output.mkdir(parents=True,exist_ok=True);build=ROOT/'build/fuzix'
    with tempfile.TemporaryDirectory(prefix='kolf-rvm-') as folder:
        work=Path(folder);files=fixtures(work,maximum=True)
        for name in ('sh','echo','cat','kill','nano-X','kolf','kolfcheck'):
            files.append(dict(name='/bin/'+name,path=str(build/(name+'.rxe')),mode=0o100755))
        # Current RXE applications can depend on the shared SDK; keep the
        # disposable disk usable for both static and shared application builds.
        for library in sorted((build/'lib').glob('*.rsl.*')):
            if library.suffix[1:].isdigit():
                files.append(dict(name='/lib/riplix/'+library.name,path=str(library),mode=0o100644))
        manifest=work/'files.json';manifest.write_text(json.dumps(files));disk=work/'disk.img'
        builder='''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {Image,writeImage} from './ports/fuzix/tools/image.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,build,output]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest)).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.alloc(0)}));
files.push({name:'/init',mode:0o100755,data:fs.readFileSync(build+'/init.rxe')},
{name:'/dev/tty',mode:0o20666,device:0x200},{name:'/dev/console',mode:0o20600,device:0x201},
{name:'/dev/null',mode:0o20666,device:0x100});
writeImage(output,fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},
{name:'ROOTFS.IMG',data:rootImage(files,65536,256)}],
{totalSectors:393216,sectorsPerFat:256,sectorsPerCluster:8,imageFactory:size=>new Image(size)}));
'''
        subprocess.run(['node','--input-type=module','-e',builder,str(manifest),str(build),str(disk)],cwd=ROOT,check=True)
        command=[str(Path(test_binary).resolve()),'--ignored','--nocapture'] if test_binary else ['cargo','test','--release','-p','rvm','--test','kolf','--','--ignored','--nocapture']
        subprocess.run(command,cwd=ROOT,check=True,
            env=dict(os.environ,KOLF_TEST_BUILD=str(build),KOLF_TEST_OUTPUT=str(output),KOLF_TEST_DISK=str(disk)))
        subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(disk)],check=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=('native','ripple','both'),default='both');p.add_argument('--rvm-test-binary');a=p.parse_args()
    if a.target in ('native','both'):native(ROOT/'build/kolf-tests/native')
    if a.target in ('ripple','both'):ripple(ROOT/'build/kolf-tests/ripple',a.rvm_test_binary)
