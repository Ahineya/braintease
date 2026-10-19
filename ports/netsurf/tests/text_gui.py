#!/usr/bin/env python3
"""Verify real Cyrillic pixels from a host-mounted text file in Fuse-X."""
import argparse
import os
from pathlib import Path
import re
import shlex
import tempfile
import time

from gui import ROOT, AuditGuest, installed_image, frame, check_disk


from font_reference import find_word


def verify_word(rgb, width, word):
    assert find_word(rgb,width,len(rgb)//(width*3),word,faces=(8,),
                     x_range=range(2,16),y_range=range(64,100)), \
        'opening Cyrillic word does not match the antialiased font pixels'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/netsurf')
    parser.add_argument('--article', type=Path, help='Also open this actual file through /mnt/host')
    args = parser.parse_args()
    build = ROOT/'fuse-x/build'
    output = args.build.resolve()/'text-gui'
    output.mkdir(parents=True, exist_ok=True)
    apps = {n: build/'apps'/(n+'.fxe') for n in ('nano-X','nxhostcheck','echo','kill','sh')}
    apps['netsurf'] = args.build.resolve()/'netsurf.fxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-text-') as temporary:
        folder = Path(temporary)
        share = folder/'share'; share.mkdir()
        # Long words, Cyrillic/Latin transitions, tabs and mixed line endings.
        text = 'Коли підтримка потрібна обом: Україна ї є ґ І Ї Є Ґ.\r\n'
        text += ('програмування'*12+' mixed ASCII text\n')*50
        (share/'unicode.txt').write_bytes(text.encode('utf-8'))
        disk = installed_image('fuse-x', build, apps, folder)
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='240',
                   FUSE_X_FRAMES=str(folder/'frames'),
                   NANOX_KEEP_TTY='1')
        previous = dict(os.environ); os.environ.update(env)
        try:
            guest = AuditGuest([build/'fuse-x','--run',disk,'--share',share], 'fuse-x', output/'guest.log')
        finally:
            os.environ.clear(); os.environ.update(previous)
        try:
            guest.raw(guest.prompt)
            guest.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            guest.shell('nano-X -p &'); guest.shell('s=$!')
            guest.wait(lambda: b'Nano-X ready' in guest.log, 'Nano-X ready')
            paths = [('unicode.txt', 'Коли')]
            if args.article:
                # A private copy preserves the user's original file exactly.
                (share/'article.txt').write_bytes(args.article.read_bytes())
                first = args.article.read_text(encoding='utf-8').lstrip().split()[0]
                paths.append(('article.txt', first))
            for name, first_word in paths:
                url = 'file:///mnt/host/'+name
                start = len(guest.log)
                guest.shell('netsurf --trace '+shlex.quote(url)+' &'); guest.shell('p=$!')
                guest.wait(lambda: ('NETSURF_GUI_DOCUMENT '+url).encode() in guest.log[start:], 'text loaded', seconds=120)
                wid = int(re.findall(rb'NETSURF_GUI_READY (\d+)', guest.log[start:])[-1])
                for width in (600, 340, 1200):
                    if width != 600:
                        guest.shell(f'nxhostcheck ctl resize {wid} {width} 420')
                    deadline = time.monotonic()+20
                    while True:
                        guest.pump()
                        _, rgb = frame(folder/'frames', 'NetSurf: '+url, (width,420),
                                       output/f'{name}-{width}.png', (255,255,255))
                        try:
                            verify_word(rgb, width, first_word)
                            break
                        except AssertionError:
                            if time.monotonic() > deadline: raise
                    # Text must extend below the first baseline, with white
                    # margins: catches zero/overflowed line height and clipping.
                    assert sum(rgb[(y*width+x)*3:(y*width+x)*3+3] == b'\0\0\0'
                               for y in range(100,300) for x in range(4,width-18)) > 100
                # Close through Nano-X and verify normal application teardown.
                guest.shell(f'nxhostcheck ctl close {wid}')
                assert b'TEXT_EXIT:0' in guest.shell('wait $p; echo TEXT_EXIT:$?')
            guest.shell('kill $s; wait $s')
            guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
            assert not any(s in guest.log for s in (b'GsError',b'panic:',b'NXR_BUFFER_FAILED',b'blit snapshot'))
        finally:
            (output/'guest.log').write_bytes(guest.log)
            guest.close()
        check_disk(disk, True)
    print('Fuse-X: host-mounted UTF-8 text, exact Cyrillic glyph pixels, resize/reflow and clean exit passed')


if __name__ == '__main__':
    main()
