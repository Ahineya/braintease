#!/usr/bin/env python3
"""Fetch bounded pinned public probe inputs; never touches installed guest disks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PIN = json.loads((HERE / 'UPSTREAM.json').read_text())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/ytmusic-extraction')
    args = ap.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    for name in ('ejs', 'youtubejs'):
        upstream = build / name
        if not upstream.exists():
            with tempfile.TemporaryDirectory(dir=build, prefix=name + '-fetch-') as temporary:
                tree = Path(temporary) / 'source'
                subprocess.run(['git', 'init', '-q', str(tree)], check=True)
                subprocess.run(['git', '-C', str(tree), 'fetch', '--depth=1', PIN[name]['repository'], PIN[name]['commit']], check=True)
                subprocess.run(['git', '-C', str(tree), 'checkout', '-q', '--detach', 'FETCH_HEAD'], check=True)
                tree.rename(upstream)
        actual = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != PIN[name]['commit']:
            raise SystemExit('Existing source does not match pin; use another --build')
    for player in PIN['players']:
        destination = build / player.get('filename', 'player-' + player['id'] + '.js')
        if destination.exists():
            data = destination.read_bytes()
        else:
            print('Fetching public player ' + player['id'], flush=True)
            with urllib.request.urlopen(player['url'], timeout=40) as response:
                data = response.read(8388609)
        if len(data) > 8388608 or hashlib.sha256(data).hexdigest() != player['sha256']:
            raise SystemExit('Player fixture hash/size mismatch: ' + player['id'])
        if not destination.exists():
            destination.write_bytes(data)
    dependencies = build / 'bundler'
    dependencies.mkdir(exist_ok=True)
    for name in ('package.json', 'package-lock.json'):
        shutil.copyfile(HERE / name, dependencies / name)
    subprocess.run(['npm', 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], cwd=dependencies, check=True)
    print('Pinned extraction inputs prepared in ' + str(build))


if __name__ == '__main__':
    main()
