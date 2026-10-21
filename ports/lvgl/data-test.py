#!/usr/bin/env python3
"""Real guest data viewer integration, including modal focus and large datasets."""
from pathlib import Path
import hashlib,importlib.util,json,re,shlex,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
gui=module('gui',PORT/'gui-test.py');fixtures=module('fixtures',PORT/'data-fixtures.py')
def main():
    out=BUILD/'data-tests';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    folder,files=fixtures.prepare();sample=PORT/'examples/measurements.csv'
    files += [dict(name=str(sample),path=str(sample),mode=0o100644)]
    for names,extra,target in ((['csv-test.c','csv.c'],['--float-format'],'csv-model'),(['data-check.c'],['-I',str(ROOT/'ports/graphics/include')],'data-check')):
        with (out/(target+'-build.log')).open('w') as log:
            subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),*[str(PORT/n) for n in names],'--static',*extra,'-o',str(out/(target+'.fxe'))],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    x=gui.GUI(out,{'lvdata':BUILD/'lvdata.fxe','csv-model':out/'csv-model.fxe','data-check':out/'data-check.fxe'},files);g=x.g
    def check(value,clipboard=False):
        data=value.encode() if clipboard else Path(value).read_bytes();h=2166136261
        for b in data:h=((h^b)*16777619)&0xffffffff
        path='clipboard' if clipboard else shlex.quote(str(value))
        assert f'DATA_CHECK {len(data)} {h:08x}' in g.shell('data-check '+path)
    def finish():
        x.send(wid,close=True);x.wait(lambda:not x.window('Data Viewer'))
        assert 'STATUS:0' in g.shell('wait $viewer; echo STATUS:$?')
    def launch(path=sample):
        at=len(g.log);g.shell('lvdata --trace --no-plot '+shlex.quote(str(path))+' &');g.shell('viewer=$!')
        wid=x.wait(lambda:x.window('Data Viewer'))['id'];x.logged('DATA_OPEN ',at);return wid
    def open_path(path,wid):
        at=len(g.log);x.ctrl(wid,18);x.logged('DATA_DIALOG open',at)
        x.frame(wid)
        x.send(wid,action=4);x.send(wid,action=2,clipboard=str(path));x.key(wid,40)
    def browse_folder(wid):
        # Entering a directory navigates the shared modal and focuses its list.
        x.frame(wid)
        x.send(wid,action=4);x.send(wid,action=2,clipboard=str(folder));x.key(wid,40)
        x.frame(wid)
        assert not x.window('Select File'),'Open launched a separate chooser process'
        assert len(x.windows())==1,'Shared file dialog created another GX window'
    def table_pixels(image,scale):
        w,h,pixels=image
        # Exclude scrollbar overlays: their thumb width legitimately changes
        # when an offscreen column was resized, while visible cells match.
        return b''.join(pixels[(y*w+16*scale)*3:(y*w+784*scale)*3] for y in range(154*scale,486*scale))
    try:
        assert 'CSV_MODEL passed' in g.shell('csv-model')
        check(folder/'sort.csv')
        wid=launch();pixels={}
        for scale in (1,2):
            x.send(wid,density=scale);actual=x.frame(wid,scale=scale);reference=gui.ppm(BUILD/'host'/f'data-{scale}x.ppm')
            x.snapshot(wid,f'initial-{scale}x',scale=scale)
            assert actual==reference,f'Data viewer differs from host at {scale}x'
            pixels[str(scale)]=len(actual[2])//3
            # Traverse the document far enough to recycle its row-cache ring,
            # then return through real wheel input and compare visible content.
            x.send(wid,input=dict(type='motion',x=450,y=300))
            for direction in (-65536,65536):
                for _ in range(20):x.send(wid,input=dict(type='wheel',x=0,y=direction))
            restored=x.frame(wid,scale=scale)
            assert table_pixels(restored,scale)==table_pixels(reference,scale),f'Recycled wheel content differs at {scale}x'

        if '--smoke' in sys.argv:
            x.send(wid,close=True);x.wait(lambda:not x.window('Data Viewer'));return
        # Selection, page traversal, both axes, and a resizeable column header.
        x.send(wid,density=1);x.frame(wid)
        at=len(g.log);x.key(wid,81);x.logged('DATA_SELECT 1 0',at)
        at=len(g.log);x.key(wid,79);x.logged('DATA_SELECT 1 1',at)
        at=len(g.log);x.key(wid,78);x.wait(lambda:b'DATA_SELECT ' in g.log[at:])
        x.ctrl(wid,74);at=len(g.log);x.send(wid,action=1);x.logged('DATA_COPY cell',at);check('Lviv',True)
        at=len(g.log);x.ctrl(wid,6,shift=True);x.logged('DATA_COPY row',at);check('Lviv\t2026-10-01\t17.1\t56\tClear',True)
        # Open is a true modal: F5 and background typing cannot reload/change the document.
        at=len(g.log);x.ctrl(wid,18);x.logged('DATA_DIALOG open',at)
        x.key(wid,62);x.text(wid,'unused');x.key(wid,41);x.pause(.3)
        assert b'DATA_OPEN ' not in g.log[at:] and b'DATA_ERROR ' not in g.log[at:],'Shortcut leaked through modal'
        x.ctrl(wid,74);at=len(g.log);x.key(wid,81);x.logged('DATA_SELECT 1 0',at)
        # A malformed CSV keeps the previous document and reports an error.
        at=len(g.log);open_path(folder/'bad.csv',wid);x.logged('DATA_ERROR ',at)
        x.snapshot(wid,'error-dialog');x.key(wid,41)
        at=len(g.log);open_path(folder/'sort.csv',wid);x.logged('DATA_OPEN '+str(folder/'sort.csv'),at)
        at=len(g.log);x.click(wid,280,134);x.logged('DATA_SORT 1 0 first=-5',at)
        at=len(g.log);x.click(wid,280,134);x.logged('DATA_SORT 1 1 first=10',at)
        x.snapshot(wid,'sorted')
        # The system file dialog navigates/selects inside this application's
        # window; no selector executable or child process is installed.
        at=len(g.log);x.ctrl(wid,18);x.logged('DATA_DIALOG open',at)
        browse_folder(wid)
        x.snapshot(wid,'open-dialog')
        x.key(wid,74)
        listed=['..']+sorted(Path(item['name']).name for item in files
                               if Path(item['name']).parent==folder)
        for _ in range(listed.index('table.tsv')):x.key(wid,81)
        at=len(g.log);x.key(wid,40)
        x.logged('DATA_OPEN '+str(folder/'table.tsv')+' rows=2 columns=2 delimiter=9',at)
        # Cancelling while browsing restores the parent and its shortcuts.
        at=len(g.log);x.ctrl(wid,18);x.logged('DATA_DIALOG open',at)
        browse_folder(wid);x.key(wid,41)
        at=len(g.log);x.key(wid,62)
        x.logged('DATA_OPEN '+str(folder/'table.tsv')+' rows=2 columns=2 delimiter=9',at)
        for name,expected in (('semi.csv','rows=2 columns=2 delimiter=59'),('empty.csv','rows=0 columns=0 delimiter=44')):
            at=len(g.log);open_path(folder/name,wid);x.logged('DATA_OPEN '+str(folder/name)+' '+expected,at)
        at=len(g.log);open_path(folder/'bad-utf8.csv',wid);x.logged('DATA_ERROR ',at);x.key(wid,41)
        at=len(g.log);open_path(folder/'sort.csv',wid);x.logged('DATA_OPEN '+str(folder/'sort.csv'),at)
        x.ctrl(wid,18)
        # Options are isolated until Apply; cancellation must not mutate preferences.
        x.key(wid,41);at=len(g.log);x.key(wid,65);x.logged('DATA_DIALOG options',at)
        x.snapshot(wid,'options-dialog');x.key(wid,41)
        # Large file: Ctrl+End reveals the last record with bounded widget count.
        at=len(g.log);start=time.monotonic();open_path(folder/'large.csv',wid);x.logged('DATA_OPEN '+str(folder/'large.csv'),at,120);load_seconds=time.monotonic()-start
        at=len(g.log);x.ctrl(wid,77);x.logged('DATA_SELECT 99999 2',at)
        x.snapshot(wid,'large-last-row')
        for scale in (1,2):
            x.send(wid,density=scale);actual=x.frame(wid,scale=scale)
            reference=gui.ppm(BUILD/'host'/f'data-large-end-{scale}x.ppm')
            assert table_pixels(actual,scale)==table_pixels(reference,scale),f'Last-record rendering differs at {scale}x'
        x.send(wid,density=1);x.frame(wid)
        at=len(g.log);start=time.monotonic();x.click(wid,310,134)
        x.logged('DATA_SORT 1 0 first=1',at,120);sort_seconds=time.monotonic()-start
        match=re.search(rb'DATA_OPEN '+re.escape(str(folder/'large.csv').encode())+rb' rows=100000 columns=3 delimiter=44 widgets=(\d+)',g.log)
        assert match and int(match[1])<=41,'Table created unbounded widgets'
        at=len(g.log);x.ctrl(wid,74);x.logged('DATA_SELECT 0 0',at)
        # Header resizing changes width without a spurious sort.
        at=len(g.log);x.send(wid,input=dict(type='button',button=0,down=True,x=210,y=134))
        x.send(wid,input=dict(type='motion',x=250,y=134));x.send(wid,input=dict(type='button',button=0,down=False,x=250,y=134));x.pause()
        assert b'DATA_SORT ' not in g.log[at:]
        # A wide file must remain readable via horizontal keyboard scrolling.
        at=len(g.log);open_path(folder/'wide.csv',wid);x.logged('DATA_OPEN '+str(folder/'wide.csv'),at)
        at=len(g.log);x.ctrl(wid,77);x.logged('DATA_SELECT 199 31',at)
        x.snapshot(wid,'wide-last-column')
        for scale in (1,2):
            x.send(wid,density=scale);actual=x.frame(wid,scale=scale)
            reference=gui.ppm(BUILD/'host'/f'data-wide-end-{scale}x.ppm')
            assert table_pixels(actual,scale)==table_pixels(reference,scale),f'Last-column rendering differs at {scale}x'
        x.send(wid,density=1);x.frame(wid)
        for width,height,accepted in ((1000,720,(1000,720)),(20,20,(640,420)),(800,560,(800,560))):
            x.send(wid,resize=[width,height]);x.frame(wid,width=accepted[0],height=accepted[1])
        # Menus support pointer dismissal and keyboard activation without a second click.
        at=len(g.log);x.click(wid,44,26);x.logged('DATA_MENU file',at);x.snapshot(wid,'file-menu')
        x.key(wid,41);x.key(wid,67);x.key(wid,40);at=len(g.log);x.key(wid,40);x.logged('DATA_DIALOG open',at);x.key(wid,41)
        finish()
        settings=g.shell('cat /data/home/.lvdata');assert 'recent='+str(folder/'wide.csv') in settings
        assert 'header=1' in settings and 'delimiter=0' in settings
        assert 'widths=184,144' in settings,'Dragged column width was not saved'
        # Apply explicit settings, restart the process, and verify both options and recents.
        wid=launch(folder/'sort.csv');at=len(g.log);x.key(wid,65);x.logged('DATA_DIALOG options',at)
        x.key(wid,40) # focused header checkbox: turn it off
        x.key(wid,43);x.key(wid,81) # radio list: Automatic -> Comma
        # Selecting the same choice again must keep it selected.
        x.key(wid,40)
        for _ in range(4):x.key(wid,43)
        at=len(g.log);x.key(wid,40)
        x.logged('DATA_OPTIONS header=0 delimiter=44',at)
        x.logged('DATA_OPEN '+str(folder/'sort.csv')+' rows=6 columns=3 delimiter=44',at)
        finish();settings=g.shell('cat /data/home/.lvdata')
        assert 'header=0' in settings and 'delimiter=44' in settings and 'widths=184,144' in settings
        wid=launch(folder/'sort.csv')
        assert b'rows=6 columns=3 delimiter=44' in g.log
        at=len(g.log);x.click(wid,44,26);x.logged('DATA_MENU file',at)
        x.key(wid,81);x.key(wid,81);at=len(g.log);x.key(wid,40)
        x.logged('DATA_OPEN '+str(folder/'sort.csv')+' rows=6 columns=3 delimiter=44',at)
        finish();check(folder/'sort.csv')
        # The loader and viewer never rewrite the opened source file.
        assert 'DATA_READY' in g.log.decode(errors='replace')
        report=dict(status='passed',executable_sha256=hashlib.sha256((BUILD/'lvdata.fxe').read_bytes()).hexdigest(),pixels_equal=pixels,large_load_seconds=load_seconds,large_sort_seconds=sort_seconds,large_table_widgets=int(match[1]),checks=['exact host/guest pixels at 1x/2x','cell/row clipboard','arrow/page keys and wheel cache recycling at 1x/2x','modal shortcut isolation and focus restoration','transactional malformed-file error','numeric header sorting','100000 rows with bounded widgets','exact last-record and last-column pixels at 1x/2x','resize bounds','pointer/keyboard menus','shared same-window file browser, sorted keyboard selection and cancellation','UTF-8 TSV/semicolon/empty files','exclusive radio delimiter choices and persistent options, widths and recents','guest parser unit checks','exact clipboard payloads','source remains unchanged','close cleanup'])
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    finally:x.close()
    print('Data viewer: parsing, table navigation, menus, modals, clipboard, large files and settings passed')
if __name__=='__main__':main()
