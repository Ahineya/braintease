#!/usr/bin/env python3
"""Real hosted GX -> Nano-X -> browser/PTY clipboard integration, no OS pasteboard changes."""
import json,os,re,shutil,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def fnv(data):
    value=2166136261
    for c in data:value=((value^c)*16777619)&0xffffffff
    return value

def main():
    build=ROOT/'fuse-x/build';apps={n:build/'apps'/f'{n}.fxe' for n in ('nano-X','nxterm','gterm','nxhostcheck','netsurf','htget','kill')}
    with tempfile.TemporaryDirectory(prefix='fuse-x-clipboard-') as folder:
        work=Path(folder);mailbox=work/'event.json';frames=work/'frames'
        saved=dict(os.environ);os.environ.update(FUSE_X_TEST_EVENT=str(mailbox),FUSE_X_FRAMES=str(frames),NANOX_KEEP_TTY='1')
        devices=[dict(name=f'/dev/{name}{i:x}',mode=0o20600,device=base+i) for i in range(16) for name,base in [('ptyp',0x300),('ttyp',0x202)]]
        try:g=Guest(work,'native',apps,[*devices,dict(name='/dev/gfx',mode=0o20600,device=0x400),
            dict(name='/page.html',text='<html><title>Clipboard page</title><style>html,body{margin:0}input{position:absolute;left:16px;top:16px;width:300px;height:32px}</style><body><input type=text value=""><p>Copy browser text here.</p></body></html>')],blocks=131072)
        finally:os.environ.clear();os.environ.update(saved)
        def wait(test,seconds=20):
            limit=time.monotonic()+seconds
            while time.monotonic()<limit:
                g.pump()
                result=test()
                if result:return result
                assert g.process.poll() is None,g.log[-4000:].decode(errors='replace')
            raise AssertionError('clipboard wait timed out:\n'+g.log[-5000:].decode(errors='replace'))
        def windows():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def window(title):return wait(lambda:next((w for w in windows() if w['visible'] and w['title']==title),None))
        def event(wid,**values):
            wait(lambda:not mailbox.exists());p=mailbox.with_suffix('.tmp');p.write_text(json.dumps(dict(window=wid,**values)));p.replace(mailbox)
            wait(lambda:not mailbox.exists())
        def file(path,data):
            expected=f'CLIPBOARD_FILE {len(data)} {fnv(data):08x}'
            out=g.shell('nxhostcheck file-check '+path)
            assert expected in out,(out,expected)
        try:
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('server=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            for terminal in ('gterm','nxterm'):
                # Burst exceeds PTY queue and both former GUI pending arrays.
                text=('abcЖ🙂\r\nxyz\t\x1b\x03\x7f'*1000)
                expected=text.replace('\r\n','\r').replace('\x1b','').replace('\x03','').replace('\x7f','').encode()
                for bracketed in (False,True):
                    payload=(b'\x1b[200~'+expected.replace(b'\r',b'\n')+b'\x1b[201~') if bracketed else expected
                    path=f'/tmp/{terminal}-{int(bracketed)}'
                    g.shell(f'{terminal} nxhostcheck paste-check {path} {len(payload)} {int(bracketed)} &');g.shell('term=$!')
                    w=window('Terminal');wid=w['id']
                    # Wait for raw-mode child greeting to be painted.
                    wait(lambda:f'CLIPBOARD_FILE 5 {fnv(b"ready"):08x}' in g.shell('nxhostcheck file-check '+path+'.ready'))
                    event(wid,action=2,clipboard=text)
                    wait(lambda:'CLIPBOARD_FILE' in g.shell('nxhostcheck file-check '+path) and f'CLIPBOARD_FILE {len(payload)} ' in g.log[-1000:].decode(errors='replace'),seconds=30)
                    file(path,payload)
                    event(wid,action=4);event(wid,action=1)
                    assert 'CLIPBOARD_CHECK 0' in g.shell('nxhostcheck clipboard get /tmp/selection')
                    output=g.shell('cat /tmp/selection');assert 'Clipboard Ж 🙂' in output,output
                    event(wid,close=True);g.shell('wait $term')
            g.shell('netsurf --trace /page.html &');g.shell('browser=$!')
            if b'NETSURF_GUI_LOADED Clipboard page' not in g.log:g.read(b'NETSURF_GUI_LOADED Clipboard page')
            wid=window('NetSurf: Clipboard page')['id']
            # Exercise the real browser core's clipboard callbacks in an HTML field.
            event(wid,input=dict(type='button',button=0,down=True,x=50,y=92))
            event(wid,input=dict(type='button',button=0,down=False,x=50,y=92))
            event(wid,action=2,clipboard='field Ж🙂')
            wait(lambda:b'NETSURF_CLIPBOARD_READ 12' in g.log)
            at=len(g.log);event(wid,action=4,clipboard='NOT_THE_FIELD');event(wid,action=1)
            wait(lambda:b'NETSURF_CLIPBOARD_WRITE 12' in g.log[at:])
            assert 'CLIPBOARD_CHECK 0' in g.shell('nxhostcheck clipboard get /tmp/field');file('/tmp/field','field Ж🙂'.encode())
            # Address focus, paste, copy and navigation use actual host input.
            event(wid,input=dict(type='key',key=224,down=True))
            event(wid,input=dict(type='key',key=15,down=True))
            event(wid,input=dict(type='key',key=15,down=False))
            event(wid,input=dict(type='key',key=224,down=False))
            at=len(g.log);event(wid,action=2,clipboard='  about:blank\r\n')
            wait(lambda:b'NETSURF_CLIPBOARD_READ 15' in g.log[at:])
            at=len(g.log);event(wid,action=4);event(wid,action=1)
            wait(lambda:b'NETSURF_CLIPBOARD_WRITE 11' in g.log[at:])
            assert 'CLIPBOARD_CHECK 0' in g.shell('nxhostcheck clipboard get /tmp/url');file('/tmp/url',b'about:blank')
            # Rejected multiline/oversized pastes preserve the current destination.
            for invalid in ('bad\naddress','x'*1024):
                at=len(g.log);event(wid,action=2,clipboard=invalid)
                wait(lambda:('NETSURF_CLIPBOARD_READ '+str(len(invalid))).encode() in g.log[at:])
                at=len(g.log);event(wid,action=4,clipboard='NOT_THE_ADDRESS');event(wid,action=1)
                wait(lambda:b'NETSURF_CLIPBOARD_WRITE 11' in g.log[at:])
                assert 'CLIPBOARD_CHECK 0' in g.shell('nxhostcheck clipboard get /tmp/url');file('/tmp/url',b'about:blank')
            # Clipboard ownership: a second connection cannot use the browser's window.
            match=re.findall(rb'NETSURF_GUI_READY (\d+)',g.log);assert match
            assert 'CLIPBOARD_CHECK 0' in g.shell('nxhostcheck clipboard foreign '+match[-1].decode())
            event(wid,input=dict(type='key',key=40,down=True));event(wid,input=dict(type='key',key=40,down=False))
            wait(lambda:b'NETSURF_GUI_DOCUMENT about:blank' in g.log)
            event(wid,close=True);out=g.shell('wait $browser; echo BROWSER_STATUS:$?');assert 'BROWSER_STATUS:0' in out,out
            g.shell('kill $server; wait $server')
            assert b'native runtime error' not in g.log and b'input queue full' not in g.log
        finally:
            for item in windows():
                if item['title'].startswith('NetSurf:') and (frames/f"{item['id']}.ppm").exists():
                    shutil.copy2(frames/f"{item['id']}.ppm",build/'clipboard-browser.ppm')
            g.close();(build/'clipboard-test.log').write_bytes(g.log)
    print('Fuse-X clipboard: both terminals, large UTF-8 paste/backpressure, bracketed paste, selection/copy, HTML-field copy/paste, address paste/navigation/rejection and ownership passed')
if __name__=='__main__':main()
