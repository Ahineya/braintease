#!/usr/bin/env python3
"""Graphical NetSurf smoke checks on disposable disks, with real Nano-X servers."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from guests import ROOT, AuditGuest, image, check_disk
sys.path.insert(0, str(ROOT / 'ports/fusede/tools'))
from build_theme import png, unpack565


def installed_image(system, build, apps, folder):
    apps=dict(apps,htget=apps['netsurf'].with_name('htget'+apps['netsurf'].suffix))
    source=image(system, build, apps, folder, [dict(name='/dev/gfx',mode=0o20600,device=0x400),
        dict(name='/ns-error-style.html',data='<!doctype html><title>Error stylesheet</title>'
             '<link rel="stylesheet" href="about:nscolours.css">'
             '<body class="ns-even-bg ns-even-fg"><h1>Error-page colours</h1>'
             '<p>The internal stylesheet and its following string argument survived.</p>')])
    output=folder/'installed.img'
    subprocess.run(['node', str(ROOT/'ports/netsurf/install.mjs'),
                    'native' if system=='fuse-x' else 'ripple', str(source),str(output),str(apps['netsurf'])],
                   cwd=ROOT,check=True)
    return output


def frame(frames, title, size, output, document_bg=(238,242,246)):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            windows = json.loads((frames / 'windows.json').read_text())
            item = next(w for w in windows if w['title'] == title and w['visible'])
            data = (frames / f"{item['id']}.ppm").read_bytes()
            match = re.match(rb'P6\n(\d+) (\d+)\n255\n', data)
            w, h = map(int, match.groups()); rgb = data[match.end():]
            if (w, h) == size and len(rgb) == w*h*3:
                # The document background and dark toolbar must both survive clipping.
                def near(x, y, expected):
                    p = (y*w+x)*3
                    return all(abs(rgb[p+i]-expected[i]) < 9 for i in range(3))
                if (near(2,2,(44,55,65)) and near(2,66,document_bg)
                    and near(w-2,70,(205,212,217))):
                    png(output, w, h, [(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
                    return item['id'], rgb
        except (FileNotFoundError, StopIteration, json.JSONDecodeError, AttributeError):
            pass
        time.sleep(.03)
    raise AssertionError(f'missing complete NetSurf frame {title} at {size}')


def native(args):
    build = args.native_build.resolve()
    apps = {n: build / 'apps' / (n+'.fxe') for n in ('nano-X','nxhostcheck','echo','kill')}
    apps['sh'] = (args.native_shell or build/'apps/sh.fxe').resolve()
    apps['netsurf'] = args.build / 'netsurf.fxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-gui-native-') as temporary:
        folder = Path(temporary); frames = folder / 'frames'; event_file=folder/'host-event.json'
        disk = installed_image('fuse-x', build, apps, folder)
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='180',
                   FUSE_X_FRAMES=str(frames), FUSE_X_TEST_EVENT=str(event_file), NANOX_KEEP_TTY='1')
        # Guest inherits the environment; keep this process's existing settings intact.
        previous = dict(os.environ); os.environ.update(env)
        try: guest = AuditGuest([build/'fuse-x','--run',disk], 'fuse-x', args.output/'native.log')
        finally: os.environ.clear(); os.environ.update(previous)
        try:
            guest.raw(guest.prompt)
            guest.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            guest.shell('nano-X -p &'); guest.shell('s=$!')
            guest.wait(lambda: b'Nano-X ready' in guest.log, 'Nano-X ready')
            guest.shell('netsurf --trace /usr/share/netsurf/index.html &'); guest.shell('p=$!')
            guest.wait(lambda: b'NETSURF_GUI_LOADED FuseDE Browser' in guest.log, 'browser loaded', seconds=120)
            _, before = frame(frames, 'NetSurf: FuseDE Browser', (600,420), args.output/'native-wide.png')
            wid = int(re.search(rb'NETSURF_GUI_READY (\d+)', guest.log).group(1))
            result = guest.shell(f'nxhostcheck ctl resize {wid} 340 420')
            assert b'ROOTLESS_CONTROL_OK' in result, result
            guest.wait(lambda: b'NETSURF_GUI_RESIZE 340 420' in guest.log, 'browser resize')
            _, after = frame(frames, 'NetSurf: FuseDE Browser', (340,420), args.output/'native-narrow.png')
            assert before != after
            def colour(rgb,w,x,y,expected):
                p=(y*w+x)*3
                return all(abs(rgb[p+i]-expected[i])<9 for i in range(3))
            assert colour(before,600,20,140,(195,214,229)), 'CSS banner background'
            assert colour(before,600,20,180,(238,242,246)), 'wide banner height'
            assert colour(after,340,20,180,(195,214,229)), 'narrow banner must wrap and become taller'
            # Use the host-event path as well as GrResizeWindow: this used to
            # deliver allocation errors directly to the browser and terminate it.
            for w,h in ((1280,800),(300,240),(1500,900),(600,420)):
                registry=json.loads((frames/'windows.json').read_text())
                host=next(item for item in registry if item['title']=='NetSurf: FuseDE Browser')
                pending=event_file.with_suffix('.tmp')
                pending.write_text(json.dumps(dict(window=host['id'],resize=[w,h])))
                pending.replace(event_file)
                frame(frames,'NetSurf: FuseDE Browser',(w,h),args.output/f'native-resize-{w}x{h}.png')
            guest.shell(f'nxhostcheck ctl close {wid}')
            result = guest.shell('wait $p; echo NETSURF_EXIT:$?')
            assert b'NETSURF_EXIT:0' in result, result
            guest.shell('netsurf --trace /ns-error-style.html &'); guest.shell('p=$!')
            guest.wait(lambda: b'NETSURF_GUI_LOADED Error stylesheet' in guest.log, 'internal error stylesheet')
            frame(frames, 'NetSurf: Error stylesheet', (600,420), args.output/'native-error-style.png', (224,224,224))
            wid = int(re.findall(rb'NETSURF_GUI_READY (\d+)', guest.log)[-1])
            guest.shell(f'nxhostcheck ctl close {wid}')
            result = guest.shell('wait $p; echo ERROR_STYLE_EXIT:$?')
            assert b'ERROR_STYLE_EXIT:0' in result, result
            guest.shell('kill $s; wait $s')
            guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
            assert not any(s in guest.log for s in (b'GsError',b'panic:',b'NXR_BUFFER_FAILED',b'blit snapshot'))
        finally: guest.close()
        check_disk(disk, True)
    print('Fuse-X: rendered pixels, clipped chrome, resize/reflow, close and clean filesystem passed')


def verify_form(words):
    from font_reference import find_word
    rgb=bytes(channel for value in words for channel in unpack565(value))
    assert find_word(rgb,640,480,'world',faces=(0,4,8)), \
        'typed HTML input does not contain world in the antialiased font'


def rvm(args):
    build = args.fuzix_build.resolve()
    apps = {n: build / (n+'.rxe') for n in ('nano-X','echo','kill')}
    apps['sh'] = (args.fuzix_shell or build/'sh.rxe').resolve()
    apps['netsurf'] = args.build / 'netsurf.rxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-gui-rvm-') as temporary:
        disk = installed_image('fuzix', build, apps, Path(temporary))
        command=([str(args.rvm_test_binary.resolve()),'--ignored','--nocapture'] if args.rvm_test_binary else
                 ['cargo','test','--release','-p','rvm','--test','netsurf','--','--ignored','--nocapture'])
        subprocess.run(command,
                       cwd=ROOT, check=True, env=dict(os.environ, NETSURF_TEST_DISK=str(disk),
                       NETSURF_TEST_OUTPUT=str(args.output), NETSURF_RVM_BUILD=str(build)))
        check_disk(disk, False)
    for source in args.output.glob('rvm-*.rgb565'):
        import struct
        data=source.read_bytes(); words=struct.unpack('<'+'H'*(len(data)//2),data)
        png(source.with_suffix('.png'),640,480,[(*unpack565(p),255) for p in words])
        if source.name=='rvm-form.rgb565': verify_form(words)
    print('Riplix/RVM: graphical browser acceptance and clean filesystem passed')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os',choices=['fuse-x','rvm','both'],default='both')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    parser.add_argument('--output',type=Path,default=ROOT/'build/netsurf/gui-tests')
    parser.add_argument('--fuzix-build',type=Path,default=ROOT/'build/fuzix')
    parser.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    parser.add_argument('--fuzix-shell',type=Path)
    parser.add_argument('--native-shell',type=Path)
    parser.add_argument('--rvm-test-binary',type=Path,help='Reuse a compiled rvm/tests/netsurf test executable')
    args=parser.parse_args(); args.build=args.build.resolve(); args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    if args.os in ('fuse-x','both'): native(args)
    if args.os in ('rvm','both'): rvm(args)
