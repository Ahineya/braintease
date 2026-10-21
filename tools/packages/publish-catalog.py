#!/usr/bin/env python3
"""Upload only catalog packages whose signed repository identity changed."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('catalogs',nargs='+',type=Path,help='catalog.json files or their directories')
    args=ap.parse_args()
    import json
    packages={}
    for path in args.catalogs:
        if path.is_dir():path=path/'catalog.json'
        for p in json.loads(path.read_text())['packages']:
            key=(p['name'],p['target'])
            if key in packages and packages[key]['sha256']!=p['sha256']:raise SystemExit(f'Conflicting package: {key}')
            packages[key]=p
    host=os.environ.get('PKG_REPO_SSH','mini.local')
    if not re.fullmatch(r'[a-zA-Z0-9.@_][a-zA-Z0-9.@_-]*',host):raise SystemExit('Invalid SSH host')
    # Use the authenticated SSH channel for the comparison too. The publication
    # script verifies the SSD UUID, archive bytes and signed resulting index.
    index=subprocess.check_output(['ssh','-o','BatchMode=yes',host,'cat /Volumes/Ahineya/RiplixRepo/public/index.v1'],text=True)
    lines=index.splitlines()
    if not lines or lines[0]!='RIPINDEX1':raise SystemExit('Invalid existing repository index')
    previous={}
    for line in lines[1:]:
        parts=line.split('\t')
        if len(parts)!=7:raise SystemExit('Invalid existing repository entry')
        previous[(parts[0],parts[2])]=parts[4]
    changed=[p for key,p in sorted(packages.items()) if previous.get(key)!=p['sha256']]
    if not changed:
        print(f'Repository is current: {len(packages)} catalog packages checked.');return
    print(f'Publishing {len(changed)} changed packages; {len(packages)-len(changed)} unchanged.',flush=True)
    subprocess.run(['sh',str(ROOT/'tools/packages/repo/publish.sh'),*[p['archive'] for p in changed]],check=True)
if __name__=='__main__':main()
