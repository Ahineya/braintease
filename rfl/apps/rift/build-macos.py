#!/usr/bin/env python3
"""Build a relocatable standalone macOS Rift distribution; no guest image is used."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[3]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,default=ROOT/'build/rift-macos')
    args=parser.parse_args()
    prefix=args.prefix.resolve()
    subprocess.run(['cargo','build','--release','-p','rfl','-p','rcc-driver'],cwd=ROOT,check=True)
    binary=prefix/'bin/rift'
    subprocess.run([ROOT/'target/release/rfl','build',ROOT/'rfl/apps/rift/main.rfl','--os','macos',
                    '--build-dir',prefix/'sdk','-o',binary],cwd=ROOT,check=True)
    data=prefix/'share/rift'
    data.mkdir(parents=True,exist_ok=True)
    (data/'themes').mkdir(parents=True,exist_ok=True)
    for name in ('turbo','graphite','mono'):
        shutil.copy2(ROOT/'rfl/apps/rift/themes'/(name+'.ini'),data/'themes'/(name+'.ini'))
    shutil.copy2(ROOT/'rfl/apps/rift/themes/config.ini',data/'config.example.ini')
    shutil.copy2(ROOT/'rfl/apps/rift/themes/theme.ini',data/'theme.example.ini')
    for name in ('hello.bf','hello.mjs'):
        shutil.copy2(ROOT/'rfl/apps/rift'/name,data/name)
    launcher=prefix/'Rift.command'
    launcher.write_text('#!/bin/sh\nRIFT_BUNDLE_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\nexec "$RIFT_BUNDLE_DIR/bin/rift" "$@"\n')
    launcher.chmod(0o755)
    (prefix/'README.txt').write_text('Rift for macOS\n\nRun bin/rift [FILES...] in your terminal.\nKeep bin/ and share/ together when moving this directory.\nSettings: ~/.riftrc and ~/.config/rift/.\nF1: help; Ctrl-Q: quit; F10: menus.\n')
    print('\nReady: '+str(binary),flush=True)


if __name__=='__main__':main()
