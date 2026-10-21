#!/usr/bin/env python3
"""Put the built collection and launcher in the Fuse-X shared games folder."""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile

PORT=Path(__file__).resolve().parent
ROOT=PORT.parents[1]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=ROOT/'build/puzzles-fuse-x')
    ap.add_argument('--destination',type=Path,default=ROOT/'fuse-x-shared/games')
    a=ap.parse_args();names=[g['id'] for g in json.loads((PORT/'catalog.json').read_text())]
    for name in ['puzzles',*names]:
        source=a.build/'apps'/f'{name}.fxe'
        if not source.is_file():raise SystemExit(f'Missing {source}; run python3 ports/puzzles/build.py first.')
    a.destination.mkdir(parents=True,exist_ok=True);collection=a.destination/'puzzles';collection.mkdir(exist_ok=True)
    def install(source,target):
        fd,temporary=tempfile.mkstemp(prefix='.'+target.name+'-',dir=target.parent);os.close(fd)
        try:
            shutil.copy2(source,temporary);os.replace(temporary,target)
        finally:
            if os.path.exists(temporary):os.unlink(temporary)
    for name in names:
        install(a.build/'apps'/f'{name}.fxe',collection/f'{name}.fxe')
        # Remove only the old, explicitly named collection locations. Other
        # games and their assets stay in games/. Atomic replacement above is
        # safe even when an older executable is still mapped by a running app.
        old=a.destination/f'{name}.fxe'
        if old.exists():old.unlink()
    install(a.build/'apps/puzzles.fxe',a.destination/'puzzles.fxe')
    for source,name in [(PORT/'README.md','README.md'),(PORT/'catalog.json','catalog.json'),
                        (PORT/'upstream/LICENCE','LICENCE'),(PORT/'upstream/puzzles.but','puzzles.but'),
                        (ROOT/'ports/graphics/fonts/LICENSE','FONT-LICENSE')]:
        install(source,collection/name)
    print(f'Staged {len(names)} games in {collection}')
    print(f'Launcher: {a.destination / "puzzles.fxe"}')


if __name__=='__main__':main()
