#!/usr/bin/env python3
"""Check OPL3 lease/copy validation and the existing PCM contract in a fresh root."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import argparse

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'ports/doom/build/guest'
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    args=parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    probe = BUILD / 'music-check.fxe'
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'),
                    str(ROOT / 'ports/doom/guest/music_check.c'), '--compiler', str(args.compiler), '-o', str(probe)], check=True)
    pcmcheck = ROOT / 'fuse-x/build/apps/pcmcheck.fxe'
    subprocess.run(['make', '-C', str(ROOT / 'fuse-x'), str(pcmcheck), f'RCC={args.compiler.resolve()}'], check=True)
    wad = (ROOT / 'ports/doom/assets/doom1.wad').read_bytes()
    count, directory = struct.unpack_from('<II', wad, 4)
    lumps = {}
    for i in range(count):
        offset, length, name = struct.unpack_from('<II8s', wad, directory + i * 16)
        lumps[name.rstrip(b'\0')] = wad[offset:offset+length]
    with tempfile.TemporaryDirectory(prefix='doom-audio-device-') as temporary:
        work = Path(temporary)
        bank, score = work / 'bank.bin', work / 'score.bin'
        bank.write_bytes(lumps[b'GENMIDI'][:6308]); score.write_bytes(lumps[b'D_E1M1'])
        guest = Guest(work, 'native', {'music-check': probe, 'pcmcheck': pcmcheck}, files=[
            dict(name='/dev/pcm', mode=0o20666, device=0x500),
            dict(name='/mnt/host/bank.bin', mode=0o100644, path=str(bank)),
            dict(name='/mnt/host/score.bin', mode=0o100644, path=str(score))])
        try:
            for name, marker in [('music-check', 'MUSIC_DEVICE_OK'), ('pcmcheck', 'pcmcheck passed')]:
                output = guest.shell(name + '; echo CHECK_STATUS:$?', seconds=60)
                assert marker in output and 'CHECK_STATUS:0' in output, output
                print(output, flush=True)
        finally:
            guest.close()
            (BUILD / 'audio-device-test.log').write_bytes(guest.log)
    print('Passed OPL3 validation, copied data across banks, inherited-FD ownership, and PCM regression')


if __name__ == '__main__':
    os.environ['RCC_NATIVE_AUDIO'] = 'off'
    main()
