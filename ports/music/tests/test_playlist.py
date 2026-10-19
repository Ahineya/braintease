#!/usr/bin/env python3
"""Playlist order/shuffle and transactional storage in disposable OS images."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple','both'],default='native')
    args=parser.parse_args()
    os.environ['RCC_NATIVE_AUDIO']='off'
    output=ROOT/'build/music-tests';output.mkdir(parents=True,exist_ok=True)
    for target in (['native','ripple'] if args.target=='both' else [args.target]):
        app=output/('playlist.fxe' if target=='native' else 'playlist.rxe')
        subprocess.run([ROOT/'target/release/rfl','build',ROOT/'ports/music/tests/playlist.rfl',
                        '--os','fuse-x' if target=='native' else 'fuzix','-o',app],cwd=ROOT,check=True)
        with tempfile.TemporaryDirectory(prefix='music-playlist-') as directory:
            guest=Guest(directory,target,{'playlist-check':app})
            try:
                result=guest.shell('playlist-check; echo CHECK_STATUS:$?',seconds=180)
                assert 'PLAYLIST_CHECKS_PASSED' in result and '\nCHECK_STATUS:0\n' in result,result
                print(target+': '+result,flush=True)
            finally:
                guest.close()
                (output/('playlist-'+target+'.log')).write_bytes(guest.log)


if __name__=='__main__':main()
