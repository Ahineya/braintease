#!/usr/bin/env python3
"""Exercise the actual RFL curses player and C engine in a private guest."""
import argparse
import fcntl
import os
from pathlib import Path
import struct
import sys
import tempfile
import termios
import time
from test_wav import ROOT, fixture

sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'libraries/tests'))
from test_qr import screen,write_png


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],default='native')
    args=parser.parse_args()
    os.environ['RCC_NATIVE_AUDIO']='off'
    build=ROOT/('fuse-x/build' if args.target=='native' else 'build/fuzix')
    app=build/('apps/music.fxe' if args.target=='native' else 'music.rxe')
    output=ROOT/'build/music-tests';output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='music-ui-') as temporary:
        directory=Path(temporary)
        track=directory/'track.wav';track.write_bytes(fixture(8000,1,16,8000*24)[0])
        short=directory/'short.wav';short.write_bytes(fixture(8000,1,16,8000*4)[0])
        guest=Guest(directory,args.target,{'music':app},files=[
            dict(name='/dev/pcm',mode=0o20666,device=0x500),
            dict(name='/library/a.wav',mode=0o100644,path=str(track)),
            dict(name='/library/b.wav',mode=0o100644,path=str(track)),
            dict(name='/library/c.wav',mode=0o100644,path=str(short)),
            dict(name='/library/d-bad.wav',mode=0o100644,text='not a WAV'),
            dict(name='/library/e.wav',mode=0o100644,path=str(short)),
            dict(name='/etc/termcap',text='ansi|vt100|xterm:co#80:li#30:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:\n')])
        active=False
        try:
            fcntl.ioctl(guest.master,termios.TIOCSWINSZ,struct.pack('HHHH',30,80,0,0))
            begin=len(guest.log)
            def visible(needle,seconds=25,row=None):
                until=time.monotonic()+seconds
                while time.monotonic()<until:
                    guest.pump()
                    cells=screen(ROOT/'build/qr-test/qr-check',bytes(guest.log[begin:]))
                    text='\n'.join(''.join(chr(ch) if 32<=ch<127 else ' ' for ch,_ in cells[y*80:(y+1)*80]) for y in range(30))
                    if needle in (text if row is None else text.splitlines()[row]):return cells,text
                raise AssertionError('Music UI did not show '+needle+'\n'+text)
            os.write(guest.master,b'music /library\r');active=True
            visible('a.wav')
            os.write(guest.master,b'\x1b[B\r')
            visible('Playing WAV')
            os.write(guest.master,b' ')
            cells,paused=visible('Paused')
            until=time.monotonic()+.7
            while time.monotonic()<until:guest.pump()
            _,still=visible('Paused')
            assert paused.splitlines()[23]==still.splitlines()[23],(paused,still)
            os.write(guest.master,b'+')
            visible('Volume 80%')
            os.write(guest.master,b'\x1b[C')
            visible('0:10')
            os.write(guest.master,b' ')
            visible('Playing WAV')
            os.write(guest.master,b'\x1b[B\r')
            cells,text=visible('/library/b.wav')
            visible('Playing WAV')
            os.write(guest.master,b'A')
            visible('Playlist 5',row=2)
            os.write(guest.master,b'A')
            visible('Added 0 track(s)')
            # Select c in the playlist. Natural completion must skip the
            # invalid WAV and play e, without another key or refresh.
            os.write(guest.master,b'\t\x1b[H\x1b[B\x1b[B\r')
            visible('/library/c.wav',row=22)
            visible('/library/e.wav',seconds=15,row=22)
            visible('Playlist finished',seconds=15)
            # Leave just the short tracks, then cross a shuffle-bag boundary.
            os.write(guest.master,b'\x1b[Hdd\x1b[Bd')
            visible('[Playlist] 2',row=2)
            os.write(guest.master,b's\x1b[H\r')
            visible('Shuffle: on',row=0)
            visible('/library/c.wav',row=22)
            visible('/library/e.wav',seconds=15,row=22)
            cells,text=visible('/library/c.wav',seconds=15,row=22)
            write_png(output/('ui-'+args.target+'.png'),cells)
            guest.at=len(guest.log)
            os.write(guest.master,b'\x1b[21~')
            guest.read(guest.prompt,25);active=False
            (output/('ui-'+args.target+'.ansi')).write_bytes(guest.log)
            guest.close()
            # Reboot the disposable filesystem, not merely a second UI process.
            guest=Guest(directory,args.target,{'music':app},reuse=True)
            fcntl.ioctl(guest.master,termios.TIOCSWINSZ,struct.pack('HHHH',30,80,0,0))
            begin=len(guest.log)
            os.write(guest.master,b'music\r');active=True
            visible('Playlist 2',row=2)
            visible('Shuffle: on',row=0)
            visible('/library',row=3)
            _,text=visible('Volume 80%',row=23)
            assert 'Playing WAV' not in text, 'Restore unexpectedly started playback'
            os.write(guest.master,b'n')
            visible('Playing WAV')
            guest.at=len(guest.log)
            os.write(guest.master,b'\x1b[21~')
            guest.read(guest.prompt,25);active=False
        finally:
            if active:
                os.write(guest.master,b'\x1b[21~');guest.at=len(guest.log);guest.read(guest.prompt,10)
            guest.close()
    print(args.target+': browsing, playback, pause, volume, seek, playlist edits, automatic advance, bad-track skipping, continuous shuffle and reboot persistence passed')


if __name__=='__main__':main()
