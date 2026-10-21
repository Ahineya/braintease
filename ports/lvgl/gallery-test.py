#!/usr/bin/env python3
"""Exercise the Graphite gallery in a fresh offline guest, including density changes."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def ppm(path):
    data=path.read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
    if not m:return None
    w,h=map(int,m.groups());pixels=data[m.end():]
    return (w,h,pixels) if len(pixels)==w*h*3 else None

def main():
    build=ROOT/'build/lvgl';out=build/'gallery-tests';out.mkdir(exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='graphite-gallery-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'event.json'
        env=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{'lvgallery':build/'lvgallery.fxe'},[dict(name='/dev/gfx',mode=0o20600,device=0x400)],blocks=65536)
        finally:os.environ.clear();os.environ.update(env)
        def wait(check,seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                g.pump();value=check()
                if value:return value
                assert g.process.poll() is None,g.log[-5000:].decode(errors='replace')
            raise AssertionError('Gallery wait timed out:\n'+g.log[-5000:].decode(errors='replace'))
        def pause(n=.3):
            end=time.monotonic()+n
            while time.monotonic()<end:g.pump()
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def window():return next((w for w in registry() if w['title']=='Graphite Gallery'),None)
        def event(**values):
            if not window():return
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp')
            tmp.write_text(json.dumps(dict(window=wid,**values)));tmp.replace(mailbox)
            wait(lambda:not mailbox.exists() or not window())
            if mailbox.exists():mailbox.unlink(missing_ok=True)
        def key(code):
            event(input=dict(type='key',key=code,down=True));event(input=dict(type='key',key=code,down=False))
        def click(x,y):
            event(input=dict(type='button',button=0,down=True,x=x,y=y));event(input=dict(type='button',button=0,down=False,x=x,y=y))
        def text(value):
            for c in value:event(input=dict(type='text',scalar=ord(c)))
        def logged(marker,at):wait(lambda:marker.encode() in g.log[at:])
        def frame(w=760,h=560,scale=1):
            def read():
                item=window()
                if not item or (item['width'],item['height'],item['scale'],item['format'])!=(w,h,scale,2):return None
                try:image=ppm(frames/f'{wid}.ppm')
                except FileNotFoundError:return None
                return image if image and image[:2]==(w*scale,h*scale) and image[2][:3]==bytes((25,26,28)) else None
            wait(read);pause(.6);return wait(read)
        def checkbox_pixels(image,scale):
            w,h,pixels=image
            return b''.join(pixels[(y*w+172*scale)*3:(y*w+194*scale)*3]
                            for y in range(360*scale,386*scale))
        def project_pixels(image,scale):
            w,h,pixels=image
            return b''.join(pixels[(y*w+172*scale)*3:(y*w+736*scale)*3]
                            for y in range(234*scale,264*scale))
        def icon_pixels(image,scale):
            w,h,pixels=image
            return b''.join(pixels[(y*w+160*scale)*3:(y*w+748*scale)*3]
                            for y in range(68*scale,522*scale))
        try:
            g.shell('unset LVGL_SCALE; lvgallery --trace &');g.shell('gallery=$!')
            wid=wait(window)['id'];comparisons={}
            for scale in (1,2):
                event(density=scale);actual=frame(scale=scale)
                reference=ppm(build/'host'/('gallery.ppm' if scale==1 else 'gallery-2x.ppm'))
                assert actual==reference,f'Gallery differs from host at {scale}x'
                comparisons[str(scale)]=len(actual[2])//3
                shutil.copyfile(frames/f'{wid}.ppm',out/f'controls-{scale}x.ppm')
                at=len(g.log);click(183,374);logged('Hidden files are visible',at)
                checked=frame(scale=scale)
                shutil.copyfile(frames/f'{wid}.ppm',out/f'checked-controls-{scale}x.ppm')
                reference=ppm(build/'host'/f'gallery-checked-{scale}x.ppm')
                assert checkbox_pixels(checked,scale)==checkbox_pixels(reference,scale),f'Checkmark differs at {scale}x'
                click(183,374);click(80,95);event(input=dict(type='motion',x=750,y=550));frame(scale=scale)
                at=len(g.log);click(300,249);logged('GALLERY_FOCUS project focused',at)
                # A drag starting in the input must not turn into a background click.
                at=len(g.log)
                event(input=dict(type='button',button=0,down=True,x=300,y=249))
                event(input=dict(type='motion',x=700,y=440))
                event(input=dict(type='button',button=0,down=False,x=700,y=440))
                pause(.2)
                assert b'GALLERY_FOCUS project blurred' not in g.log[at:],'Drag blurred the input'
                at=len(g.log);click(700,440);logged('GALLERY_FOCUS project blurred',at)
                # Both the visible caret/selection and the keyboard/clipboard target
                # must disappear; clearing just LV_STATE_FOCUSED would leave typing live.
                at=len(g.log);text('z');event(action=2,clipboard='unwanted');key(42);key(40);pause(.2)
                assert not any(v in g.log[at:] for v in (b'GALLERY_TEXT ',b'GALLERY_VALIDATION_TEXT ',b'Project name updated')),'Blurred input accepted editing'
                idle=frame(scale=scale)
                baseline=ppm(build/'host'/('gallery.ppm' if scale==1 else 'gallery-2x.ppm'))
                assert project_pixels(idle,scale)==project_pixels(baseline,scale),'Blur left a caret, selection or focus border'
                # From no focus, forward Tab starts at the first enabled control.
                at=len(g.log);key(43);key(40);logged('GALLERY_PAGE 0',at)
                # Reverse traversal starts at the last enabled control and skips
                # the disabled checkbox. Return it to the original unchecked state.
                click(700,440);at=len(g.log);event(input=dict(type='key',key=225,down=True));key(43)
                event(input=dict(type='key',key=225,down=False));key(40);logged('Hidden files are visible',at)
                key(40)
                at=len(g.log);click(300,311);logged('GALLERY_FOCUS validation focused',at)
                event(action=4);at=len(g.log);event(action=2,clipboard='/');logged('GALLERY_VALIDATION valid',at)
                at=len(g.log);text('!');logged('GALLERY_VALIDATION_TEXT /!',at)
                at=len(g.log);key(42);logged('GALLERY_VALIDATION valid',at)
                event(action=1);event(action=3);event(action=2)
                at=len(g.log);click(700,440);logged('GALLERY_FOCUS validation blurred',at)
                at=len(g.log);event(action=2,clipboard='unwanted');text('q');pause(.2)
                assert b'GALLERY_VALIDATION_TEXT ' not in g.log[at:] and b'GALLERY_VALIDATION ' not in g.log[at:],'Blurred validation field accepted editing'
                click(300,311);event(action=4);at=len(g.log);event(action=2,clipboard='/missing/folder')
                logged('GALLERY_VALIDATION invalid',at)
                # Test that pointer/Tab transfers reset replace-selection state,
                # without changing either field's contents on navigation.
                event(action=4);key(43);key(43);event(input=dict(type='key',key=225,down=True));key(43);key(43)
                event(input=dict(type='key',key=225,down=False))
                at=len(g.log);key(77);text('!');logged('GALLERY_VALIDATION_TEXT /missing/folder!',at)
                key(42);click(80,95);event(input=dict(type='motion',x=750,y=550));frame(scale=scale)
            event(density=1);frame()
            before=frame()[2];event(input=dict(type='motion',x=310,y=124));pause(.3)
            assert frame()[2]!=before,'Hover did not change the primary action'
            at=len(g.log);click(310,124);logged('GALLERY_STATUS Primary action selected',at)
            at=len(g.log);click(400,124);pause(.3)
            assert b'Unavailable action selected' not in g.log[at:],'Disabled action activated'
            click(300,249);event(action=4);event(action=2,clipboard='Український проєкт')
            at=len(g.log);text('!');logged('GALLERY_TEXT Український проєкт!',at)
            key(42);at=len(g.log);text('?');logged('GALLERY_TEXT Український проєкт?',at)
            event(action=1);event(action=3);event(action=2)
            key(43);key(43);at=len(g.log);key(40);logged('GALLERY_STATUS Hidden files are visible',at)
            click(300,311);event(action=4);at=len(g.log);event(action=2,clipboard='/');logged('GALLERY_VALIDATION valid',at)
            at=len(g.log);key(59);logged('GALLERY_PAGE 1',at);frame()
            shutil.copyfile(frames/f'{wid}.ppm',out/'typography-1x.ppm')
            event(density=2);frame(scale=2)
            shutil.copyfile(frames/f'{wid}.ppm',out/'typography-2x.ppm')
            at=len(g.log);key(60);logged('GALLERY_PAGE 2',at);frame(scale=2)
            click(230,155);at=len(g.log);key(77);logged('Selected: document-48.txt',at)
            at=len(g.log);key(74);logged('Selected: document-01.txt',at)
            at=len(g.log);key(81);logged('Selected: document-02.txt',at)
            event(input=dict(type='motion',x=400,y=300));before=frame(scale=2)[2]
            event(input=dict(type='wheel',x=0,y=-65536));pause(.3)
            assert frame(scale=2)[2]!=before,'List wheel did not scroll'
            shutil.copyfile(frames/f'{wid}.ppm',out/'lists-2x.ppm')
            at=len(g.log);key(61);logged('GALLERY_PAGE 3',at)
            for scale in (1,2):
                event(density=scale);event(input=dict(type='motion',x=750,y=550))
                actual=frame(scale=scale);reference=ppm(build/'host'/f'gallery-icons-{scale}x.ppm')
                shutil.copyfile(frames/f'{wid}.ppm',out/f'icons-{scale}x.ppm')
                assert icon_pixels(actual,scale)==icon_pixels(reference,scale),f'Lucide host/guest pixels differ at {scale}x'
                # Click the artwork itself: it must route to its button, rather
                # than taking focus or intercepting the action. Enter toggles back.
                at=len(g.log);click(200,124);logged('GALLERY_STATUS Folder icon',at)
                at=len(g.log);key(40);logged('GALLERY_STATUS File icon',at)
                at=len(g.log);click(436,124);pause(.2)
                assert b'Delete selected' not in g.log[at:],'Disabled icon button activated'
                at=len(g.log);click(335,124);logged('GALLERY_STATUS Open selected',at)
                # Rebuild the page to restore the reference state at the next density.
                key(61)
            event(density=2);frame(scale=2)
            event(resize=[20,20]);frame(640,460,2)
            event(density=1);frame(640,460,1)
            event(resize=[760,560]);frame()
            at=len(g.log);key(58);logged('GALLERY_PAGE 0',at);frame()
            click(300,311);key(77);at=len(g.log);text('!');logged('GALLERY_VALIDATION_TEXT /!',at)
            event(action=4);event(action=2,clipboard='/missing/folder')
            click(300,249);key(77);at=len(g.log);text('!');logged('GALLERY_TEXT Український проєкт?!',at)
            event(density=2);frame(scale=2)
            # Focus a non-editing navigation control; then check idle publication.
            click(80,95);frame(scale=2);pause(.6)
            checked=frame(scale=2)
            assert checkbox_pixels(checked,2)==checkbox_pixels(ppm(build/'host/gallery-checked-2x.ppm'),2), 'Restored checkbox differs'
            shutil.copyfile(frames/f'{wid}.ppm',out/'edited-controls-2x.ppm')
            stamp=(frames/f'{wid}.ppm').stat().st_mtime_ns;pause(1.3)
            assert (frames/f'{wid}.ppm').stat().st_mtime_ns==stamp,'Idle gallery keeps publishing'
            key(41);wait(lambda:not window());pause()
            assert 'STATUS:0' in g.shell('wait $gallery; echo STATUS:$?')
            assert not any(s in g.log for s in (b'native runtime error',b'panic:',b'Assertion `',b'graphics operation failed'))
            report=dict(status='passed',pixels_equal=comparisons,
                        executable_sha256=hashlib.sha256((build/'lvgallery.fxe').read_bytes()).hexdigest(),
                        checks=['exact full-colour host/guest pixels at 1x/2x','hover and primary action','disabled action',
                                'Unicode editing and clipboard','keyboard checkbox and exact checkmark pixels','page and draft preservation',
                                'blank-panel blur at 1x/2x','no typing or paste after blur','drag retains focus',
                                'forward/reverse Tab after blur','editable validation and live folder checks','focus transfer clears selection',
                                'exact Lucide masks and state colours at 1x/2x','named icon replacement','icon button mouse and keyboard routing',
                                'list keyboard and wheel','density changes','minimum resize','idle publication','close cleanup'])
        finally:
            try:g.close()
            finally:(out/'guest.log').write_bytes(g.log)
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Graphite gallery: guest controls, typography, lists, density and lifecycle passed')

if __name__=='__main__':main()
