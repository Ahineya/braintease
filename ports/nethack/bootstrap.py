#!/usr/bin/env python3
"""Bootstrap NetHack's symbolic headers with a host compiler (no host game)."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent
SHA256 = '98cf67df6debf9668a61745aa84c09bcab362e5d33f5b944ec5155d44d2aacb2'


def main():
    archive = PORT / 'vendor/nethack-367-src.tgz'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('NetHack release archive checksum mismatch')
    reference = PORT / 'build/reference'
    reference.mkdir(parents=True,exist_ok=True)
    for folder in ['include','src','util','dat']:
        shutil.copytree(PORT / 'upstream' / folder,reference / folder,dirs_exist_ok=True)
    prelude = reference / 'host-prelude.h'
    prelude.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n#include <string.h>\n#include <time.h>\n')
    command = ['clang','-std=gnu89','-Wno-deprecated-non-prototype','-Wno-strict-prototypes','-Wno-macro-redefined','-include',str(prelude),'-I../include','-DNOMAIL','-DNOTPARMDECL','makedefs.c','../src/monst.c','../src/objects.c','-o','makedefs']
    apple_ld = Path('/Library/Developer/CommandLineTools/usr/bin/ld')
    if sys.platform == 'darwin' and apple_ld.exists(): command.insert(1,'-fuse-ld=' + str(apple_ld))
    subprocess.run(command,cwd=reference / 'util',check=True)
    for option in ['p','o','z','v']:
        subprocess.run(['./makedefs','-'+option],cwd=reference/'util',check=True)
    print('Generated symbolic bootstrap headers; target data still requires generate.py.')

if __name__ == '__main__': main()
