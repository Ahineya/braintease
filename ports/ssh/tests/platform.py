#!/usr/bin/env python3
"""Build and exercise the SSH filesystem adapters inside a disposable guest."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    args=parser.parse_args()
    executable=ROOT/'build/ssh'/('platformcheck.fxe' if args.target=='native' else 'platformcheck.rxe')
    subprocess.run([sys.executable,str(ROOT/'ports/ssh/app.py'),'--target',args.target,
                    '--source',str(Path(__file__).with_name('platformcheck.c')),
                    '-o',str(executable)],check=True)
    with tempfile.TemporaryDirectory(prefix='ssh-platform-',dir='/tmp') as directory:
        guest=Guest(directory,args.target,{'platformcheck':executable})
        try:
            text=guest.shell('platformcheck; echo PLATFORM_STATUS:$?')
            assert 'YYYYYYYYY\n' in text and 'PLATFORM_STATUS:0\n' in text,text
            print('PASS 64-bit SFTP decimal sizes, padding and buffer bounds',flush=True)
        finally:
            guest.close()


if __name__=='__main__':main()
