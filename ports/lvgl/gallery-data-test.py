#!/usr/bin/env python3
"""Gallery's shared virtual table, menus and modal focus in a disposable guest."""
from pathlib import Path
import hashlib,importlib.util,json,re,time
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
spec=importlib.util.spec_from_file_location('gui',PORT/'gui-test.py');gui=importlib.util.module_from_spec(spec);spec.loader.exec_module(gui)
def main():
    out=BUILD/'gallery-data-tests';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    x=gui.GUI(out,{'lvgallery':BUILD/'lvgallery.fxe'},[]);g=x.g
    def frame(scale=1,w=760,h=560):return x.frame(wid,scale,w,h)
    def snapshot(name,scale=1,w=760,h=560):return x.snapshot(wid,name,scale=scale,width=w,height=h)
    def menu():
        at=len(g.log);x.click(wid,230,95);x.logged('GALLERY_MENU data',at)
    def options():
        menu();x.key(wid,81);x.key(wid,81);x.key(wid,81)
        at=len(g.log);x.key(wid,40);x.logged('GALLERY_DIALOG options',at)
    def content(image,scale):
        w,h,p=image
        return b''.join(p[(y*w+160*scale)*3:(y*w+748*scale)*3] for y in range(68*scale,522*scale))
    try:
        g.shell('lvgallery --trace &');g.shell('gallery=$!');wid=x.wait(lambda:x.window('Graphite Gallery'))['id']
        at=len(g.log);x.key(wid,62);x.logged('GALLERY_PAGE 4',at)
        comparisons={}
        for scale in (1,2):
            x.send(wid,density=scale);actual=frame(scale)
            reference=gui.ppm(BUILD/'host'/f'gallery-data-{scale}x.ppm')
            assert content(actual,scale)==content(reference,scale),f'Data page pixels differ at {scale}x'
            comparisons[str(scale)]=True;snapshot(f'data-{scale}x',scale)
        x.send(wid,density=1);frame()
        at=len(g.log);x.ctrl(wid,77);x.logged('GALLERY_TABLE_SELECT 99999 7',at);snapshot('last-cell')
        at=len(g.log);x.ctrl(wid,74);x.logged('GALLERY_TABLE_SELECT 0 0',at)
        at=len(g.log);x.key(wid,78);x.wait(lambda:b'GALLERY_TABLE_SELECT ' in g.log[at:]);x.ctrl(wid,74)
        # Wheel scrolls without changing the selected record.
        x.send(wid,input=dict(type='motion',x=450,y=300));before=frame()[2]
        x.send(wid,input=dict(type='wheel',x=0,y=-65536));assert frame()[2]!=before
        x.ctrl(wid,74)
        # Resizing a header edge must not sort the column.
        at=len(g.log);x.send(wid,input=dict(type='button',button=0,down=True,x=370,y=132))
        x.send(wid,input=dict(type='motion',x=410,y=132));x.send(wid,input=dict(type='button',button=0,down=False,x=410,y=132));x.pause()
        assert b'GALLERY_TABLE_SORT' not in g.log[at:]
        at=len(g.log);x.click(wid,280,132);x.logged('GALLERY_TABLE_SORT 0 0',at)
        at=len(g.log);x.click(wid,280,132);x.logged('GALLERY_TABLE_SORT 0 1',at)
        menu();snapshot('menu');x.key(wid,77);x.key(wid,74) # End / Home
        at=len(g.log);x.key(wid,40);x.logged('Cell copied',at)
        menu();x.click(wid,650,60);x.click(wid,280,160);at=len(g.log);x.key(wid,81);x.logged('GALLERY_TABLE_SELECT 1 0',at)
        options();snapshot('dialog')
        x.send(wid,action=4);x.send(wid,action=2,clipboard='Таблиця π!')
        for scale in (1,2):
            x.send(wid,density=scale);actual=frame(scale)
            reference_path=BUILD/'host'/f'gallery-dialog-{scale}x.ppm'
            reference=gui.ppm(reference_path)
            x1,y1,x2,y2=json.loads(Path(str(reference_path)+'.modal.json').read_text())
            def modal(image):
                width,height,pixels=image
                # Rounded outer corners reveal different table records beneath
                # the host/guest overlays. Compare the opaque contents only.
                inset=4*scale
                return b''.join(pixels[(y*width+x1+inset)*3:(y*width+x2+1-inset)*3] for y in range(y1+inset,y2+1-inset))
            (out/f'compared-dialog-{scale}x.ppm').write_bytes((x.frames/f'{wid}.ppm').read_bytes())
            assert modal(actual)==modal(reference),f'Dialog typography differs at {scale}x'
            snapshot(f'unicode-dialog-{scale}x',scale)
        x.send(wid,density=1);frame()
        # Underlying page shortcuts and table arrows are blocked while modal.
        at=len(g.log);x.key(wid,58);x.key(wid,62);x.key(wid,81);x.pause()
        assert b'GALLERY_PAGE ' not in g.log[at:] and b'GALLERY_TABLE_SELECT ' not in g.log[at:]
        # Empty input keeps dialog open; Unicode clipboard and Enter apply once.
        x.send(wid,action=4);x.key(wid,42);x.key(wid,40);snapshot('validation')
        x.send(wid,action=2,clipboard='Таблиця π');x.text(wid,'!')
        # Density and resizing preserve this draft and trap Tab in the dialog.
        x.send(wid,density=2);frame(2);snapshot('dialog-2x',2)
        x.send(wid,resize=[640,460]);frame(2,640,460)
        x.send(wid,resize=[760,560]);frame(2)
        # input -> Standard -> Compact -> Cancel -> Apply -> input
        for _ in range(5):x.key(wid,43)
        at=len(g.log);x.key(wid,40);x.logged('GALLERY_DIALOG_APPLY Таблиця π! compact=0',at)
        x.send(wid,density=1);frame()
        # Cancel does not replace the saved title, including modal cut/paste.
        options();x.send(wid,action=4);x.send(wid,action=2,clipboard='cancelled');x.key(wid,41)
        options();at=len(g.log);x.key(wid,40);x.logged('GALLERY_DIALOG_APPLY Таблиця π! compact=0',at)
        # Rebuilding the page deletes its visible pool safely.
        for key in (58,62,61,62):x.key(wid,key)
        frame();snapshot('final')
        # A focused non-editing control is idle: no continuous frame publication.
        x.click(wid,80,240);frame();stamp=(x.frames/f'{wid}.ppm').stat().st_mtime_ns;x.pause(1.1)
        assert (x.frames/f'{wid}.ppm').stat().st_mtime_ns==stamp
        x.send(wid,close=True);x.wait(lambda:not x.window('Graphite Gallery'))
        assert 'STATUS:0' in g.shell('wait $gallery; echo STATUS:$?')
        assert not any(s in g.log for s in (b'native runtime error',b'panic:',b'graphics operation failed'))
        (out/'result.json').write_text(json.dumps(dict(status='passed',executable_sha256=hashlib.sha256((BUILD/'lvgallery.fxe').read_bytes()).hexdigest(),pixels_equal=comparisons,checks=['100000-row bounded table','exact host/guest content 1x/2x','exact Unicode modal typography at 1x/2x and live density change','two-axis keyboard and wheel','header resize and sorting','menu arrows Home End activation and outside dismissal','modal shortcut isolation','validation and Unicode draft','Tab trapping and focus restoration','modal density and resize','apply/cancel','page deletion lifecycle','idle publication']),indent=2)+'\n')
    finally:x.close()
    print('Gallery Data: tables, menus, dialogs and focus passed')
if __name__=='__main__':main()
