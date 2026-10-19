#!/usr/bin/env python3
"""Real browser status progress for sized and close-delimited responses."""
import json,os,re,tempfile,time
from pathlib import Path
from cache import Services,ROOT
from fixtures import make
from guest import Guest
with tempfile.TemporaryDirectory(prefix='netsurf-progress-') as name:
    work=Path(name);peer=Services(work/'nic',make(work/'certs'));build=ROOT/'fuse-x/build'
    apps={n:build/'apps'/(n+'.fxe') for n in ['nano-X','nxhostcheck','kill']}
    apps.update({n:ROOT/'build/netsurf'/(n+'.fxe') for n in ['netsurf','htget']})
    previous=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(work/'frames'),NANOX_KEEP_TTY='1')
    try:g=Guest(work,'native',apps,[dict(name='/dev/gfx',mode=0o20600,device=0x400),dict(name='/usr/share/netsurf/web.nfa',path=str(ROOT/'ports/netsurf/fonts/web.nfa'))],peer,blocks=131072)
    finally:os.environ.clear();os.environ.update(previous)
    try:
        help_text=g.shell('netsurf --help; echo POLICY_HELP:$?')
        assert 'Images and JavaScript are off by default' in help_text and 'POLICY_HELP:0' in help_text,help_text
        js_text=g.shell('netsurf --javascript; echo POLICY_JS:$?')
        assert 'JavaScript is not available' in js_text and 'POLICY_JS:2' in js_text,js_text
        g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('s=$!')
        if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
        for kind,pattern in [('known',rb'Receiving \d+ / \d+ bytes \(\d+%\)'),('unknown',rb'Receiving \d+ bytes \(size unknown\)')]:
            start=len(g.log);g.shell('netsurf --trace http://tls.test/progress-'+kind+' &');g.shell('p=$!')
            until=time.monotonic()+90;captured=False
            while not re.search(pattern,g.log[start:]):
                g.pump();assert time.monotonic()<until,g.log[-5000:].decode(errors='replace')
            # A real frame must show the green progress line during a transfer.
            while time.monotonic()<until and not captured:
                g.pump()
                try:
                    item=next(w for w in json.loads((work/'frames/windows.json').read_text()) if w['visible'])
                    data=(work/f"frames/{item['id']}.ppm").read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    w,h=map(int,m.groups());rgb=data[m.end():]
                    captured=any(abs(rgb[((h-1)*w+x)*3]-89)<9 and abs(rgb[((h-1)*w+x)*3+1]-190)<9 for x in range(w))
                except (FileNotFoundError,StopIteration,AttributeError,json.JSONDecodeError):pass
            assert captured,'visible progress bar missing'
            if b'NETSURF_GUI_LOADED Progress' not in g.log[start:]:g.read(b'NETSURF_GUI_LOADED Progress',90)
            wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',g.log[start:])[-1]);g.shell(f'nxhostcheck ctl close {wid}')
            assert 'PROGRESS_STATUS:0' in g.shell('wait $p; echo PROGRESS_STATUS:$?')
        g.shell('kill $s; wait $s')
    finally:
        (ROOT/'build/netsurf/progress-native.log').write_bytes(g.log);g.close();peer.close()
print('Fuse-X: live byte counts, known-length percentage, unknown-length activity and painted progress bars passed')
