#!/usr/bin/env python3
"""Stage the optional native game in a host share; never modify a guest disk."""
import argparse
import os
from pathlib import Path
import shutil
import tempfile

PORT=Path(__file__).resolve().parent


def stage(destination):
    destination=Path(destination).resolve()
    binary=PORT/'build/fuse-x/nethack.fxe'
    data=PORT/'build/nethack-data'
    if not binary.exists() or not (data/'dungeon').exists():
        raise RuntimeError('Build the game and its target data before staging.')
    destination.mkdir(parents=True,exist_ok=True)
    def copy(source,target):
        target.parent.mkdir(parents=True,exist_ok=True)
        fd,name=tempfile.mkstemp(prefix='.nethack-',dir=target.parent)
        os.close(fd)
        try:
            shutil.copy2(source,name)
            os.replace(name,target)
        finally:
            if os.path.exists(name): os.unlink(name)
    copy(binary,destination/'nethack.fxe')
    graphical=PORT/'build/fuse-x-nx/nethack-nx.fxe'
    if graphical.exists(): copy(graphical,destination/'nethack-nx.fxe')
    for source in data.iterdir():
        if source.is_file(): copy(source,destination/'nethack-data'/source.name)
    copy(PORT/'upstream/dat/license',destination/'nethack-LICENSE.txt')
    copy(PORT/'REFERENCE.txt',destination/'nethack-README.txt')
    print('Staged NetHack at '+str(destination/'nethack.fxe'))
    if graphical.exists(): print('Staged graphical NetHack at '+str(destination/'nethack-nx.fxe'))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination',nargs='?',type=Path,default=PORT.parents[1]/'fuse-x-shared')
    stage(parser.parse_args().destination)
