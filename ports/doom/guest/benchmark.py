#!/usr/bin/env python3
"""Measure guest Doom audio modes without screenshots or personal disk images."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=106)
    parser.add_argument('--live', action='store_true')
    parser.add_argument('--fast', action='store_true', help='disable tick pacing to check nonblocking audio backpressure')
    parser.add_argument('--sample', type=Path, help='macOS stack-sampling report for the first mode')
    parser.add_argument('--size', nargs=2, type=int, metavar=('WIDTH','HEIGHT'), help='exercise a resized viewport without saving screenshots')
    parser.add_argument('--check', action='store_true', help='require near-35 Hz gameplay and no logical underruns')
    parser.add_argument('--output', type=Path, default=ROOT / 'ports/doom/build/guest/benchmark.json')
    parser.add_argument('--modes', nargs='+', choices=['silent', 'effects', 'music', 'both'], default=['silent', 'effects', 'music', 'both'])
    args = parser.parse_args()
    os.environ['RCC_NATIVE_AUDIO'] = 'on' if args.live else 'off'
    for name in ['FUSE_X_FRAMES', 'FUSE_X_FRAME', 'RCC_NATIVE_AUDIO_WAV']:
        os.environ.pop(name, None)
    results = []
    with tempfile.TemporaryDirectory(prefix='doom-benchmark-') as temporary:
        event=Path(temporary)/'event.json'
        if args.size: os.environ['FUSE_X_TEST_EVENT']=str(event)
        guest = Guest(temporary, 'native', {'doom': ROOT / 'ports/doom/build/guest/fuse-x/doom.fxe'}, files=[
            dict(name='/dev/gfx', mode=0o20666, device=0x400),
            dict(name='/dev/pcm', mode=0o20666, device=0x500),
            dict(name='/doom1.wad', mode=0o100644, path=str(ROOT / 'ports/doom/assets/doom1.wad'))])
        try:
            for mode in args.modes:
                if args.size:
                    # This private VM opens one Doom window per invocation;
                    # GX IDs start at one and are never reused.
                    event.write_text(json.dumps(dict(window=len(results)+1,resize=args.size)))
                option = dict(silent='-nosound', effects='-nomusic', music='-nosfx', both='')[mode]
                start = time.monotonic()
                sampling = []
                def sample():
                    if guest.process.poll() is not None: return
                    sampling.append(subprocess.Popen(['/usr/bin/sample', str(guest.process.pid), '5', '1', '-file', str(args.sample.resolve())],
                                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                timer = threading.Timer(1.5, sample) if args.sample and not results else None
                if timer: timer.start()
                output = guest.shell(f'doom -iwad /doom1.wad -iwaddemo demo1 --frames {args.frames} --stats {option} {"--fast" if args.fast else ""}; echo STATUS:$?', seconds=180)
                if timer: timer.cancel(); timer.join()
                for process in sampling: process.wait(timeout=15)
                assert 'STATUS:0' in output and 'DOOM_STATS' in output, output
                result = dict(mode=mode, total_seconds=round(time.monotonic()-start, 3))
                for line in output.splitlines():
                    if line.startswith(('DOOM_STATS ', 'DOOM_AUDIO ', 'DOOM_SPEAKER ')):
                        result.update({('pcm_frames' if key == 'frames' and line.startswith('DOOM_AUDIO ') else key): int(value)
                                       for key, value in re.findall(r'(\w+)=(\d+)', line)})
                result['fps'] = round((args.frames-1)*1000/result['elapsed_ms'], 2)
                if args.size: assert (result.get('width'),result.get('height')) == tuple(args.size), result
                results.append(result)
                print(json.dumps(result), flush=True)
                if args.check:
                    assert result['fps'] >= 33, result
                    assert not result.get('underruns', 0), result
                    if not args.fast:
                        assert not result.get('dropped', 0), result
                    if args.live and mode != 'silent':
                        assert result.get('live') == 1 and result.get('max_gap_ms', 0) <= 10, result
        finally:
            guest.close()
            args.output.with_suffix('.log').write_bytes(guest.log)
            args.output.write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
