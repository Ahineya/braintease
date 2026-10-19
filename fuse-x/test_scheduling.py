#!/usr/bin/env python3
"""Check native exec/CPU/mux latency and physical speaker starvation on a private disk.

Requires existing sh/echo/cat/lynx/mux packages. --live-audio uses the real speaker
callback with a silent WAV; the default exercises logical PCM without hardware.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live-audio', action='store_true')
    parser.add_argument('--max-gap-ms', type=int, default=200)
    parser.add_argument('--max-speaker-gap-ms', type=int, default=100)
    args = parser.parse_args()
    os.environ['RCC_NATIVE_AUDIO'] = 'on' if args.live_audio else 'off'
    output = ROOT / 'build/scheduling'
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT/'fuse-x/cc.py'),
                    str(ROOT/'fuse-x/examples/scheduling.c'), str(ROOT/'ports/music/wav.c'),
                    '-I', str(ROOT/'ports/music'), '-o', str(output/'scheduling.fxe')], check=True)
    apps = {name: ROOT/'fuse-x/build/apps'/f'{name}.fxe' for name in ('lynx', 'mux')}
    apps['sched'] = output/'scheduling.fxe'
    for path in apps.values():
        if not path.is_file():
            raise SystemExit(f'Missing {path}; build the Fuse-X applications first')
    with tempfile.TemporaryDirectory(prefix='fuse-x-scheduling-') as temporary:
        directory = Path(temporary)
        # Silent PCM still exercises the physical callback without playing noise.
        data = bytes(8000 * 20 * 2)
        wave = directory/'silence.wav'
        wave.write_bytes(b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+
            struct.pack('<IHHIIHH',16,1,1,8000,16000,2,16)+b'data'+struct.pack('<I',len(data))+data)
        files = [dict(name='/dev/pcm', mode=0o20666, device=0x500),
                 dict(name='/track.wav', path=str(wave)),
                 dict(name='/etc/lynx.cfg', path=str(ROOT/'ports/lynx/fixtures/lynx.cfg')),
                 dict(name='/page.html', text='<html><body>Scheduling page</body></html>')]
        # The same PTY inventory as an ordinary installed disk.
        ptys = subprocess.check_output(['node','--input-type=module','-e',
            "import {ptyDevices} from './ports/fuzix/tools/pty.mjs';console.log(JSON.stringify(ptyDevices()));"],cwd=ROOT,text=True)
        files += json.loads(ptys)
        g = Guest(directory, 'native', apps, files, blocks=65536)
        try:
            g.shell('sched latency > /latency &')
            g.shell('sched audio /track.wav > /audio &')
            end = time.monotonic()+1
            while time.monotonic()<end:g.pump()
            print('Scheduling: starting Lynx twice during PCM playback', flush=True)
            for _ in range(2):
                result = g.shell('lynx -dump /page.html > /page.out; echo EXEC_STATUS:$?')
                assert 'EXEC_STATUS:0' in result, result
            print('Scheduling: concurrent exec, CPU-bound code and mux redraws', flush=True)
            g.shell('lynx -dump /page.html > /page.out &')
            g.shell('sched cpu > /cpu &')
            result = g.shell('mux new stress sched terminal', seconds=60)
            assert 'TERMINAL_DONE' in result, result[-3000:]
            g.shell('wait', seconds=60)
            result = g.shell('cat /latency; cat /audio; cat /cpu; cat /page.out')
            print(result, flush=True)
            latency = re.search(r'LATENCY_DONE max_gap_ms=(\d+) samples=(\d+)',result)
            audio = re.search(r'AUDIO_DONE logical=(\d+) late_ms=(\d+) live=(\d+) host_gaps=(\d+) host_max_gap_ms=(\d+) host_missing_frames=(\d+)',result)
            assert latency and audio and 'CPU_DONE' in result and 'Scheduling page' in result, result
            assert int(latency[1]) <= args.max_gap_ms, result
            assert int(latency[2]) > 100, result
            assert int(audio[1]) == 0 and int(audio[2]) == 0, result
            if args.live_audio:
                assert int(audio[3]) == 1, 'No physical audio device; live test was not exercised'
                assert int(audio[5]) <= args.max_speaker_gap_ms, result
            else:
                assert tuple(map(int,audio.groups()[2:])) == (0,0,0,0), result
        finally:
            (output/'guest.log').write_bytes(g.log)
            g.close()
    print('Scheduling: bounded wakeups, successful exec and uninterrupted PCM timeline passed', flush=True)


if __name__ == '__main__':
    main()
