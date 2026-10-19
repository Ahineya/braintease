#!/usr/bin/env python3
"""Compare FXE sizes and count each required shared library once."""
import argparse
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parent.parent


def package(path):
    data=path.read_bytes()
    if len(data)<16 or data[:8]!=b'FUSEX001':raise ValueError(f'not an FXE/FXL: {path}')
    manifest,native=struct.unpack_from('<II',data,8)
    metadata=json.loads(data[16:16+manifest])
    return dict(total=len(data),manifest=manifest,data=len(data)-16-manifest-native,native=native),metadata


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'fuse-x/build/shared-test')
    p.add_argument('--baseline',type=Path,default=ROOT/'fuse-x/build/shared-size-baseline.json')
    p.add_argument('--capture-baseline',type=Path,help='record packages from this build before converting them')
    p.add_argument('--json',type=Path)
    p.add_argument('names',nargs='*',default=['chat','htget','lynx','gterm','mux'])
    args=p.parse_args()
    if args.capture_baseline:
        baseline={}
        for name in args.names:
            path=args.capture_baseline/'apps'/f'{name}.fxe'
            if not path.exists():path=args.capture_baseline/f'{name}.fxe'
            baseline[name]=package(path)[0]
        args.baseline.parent.mkdir(parents=True,exist_ok=True)
        args.baseline.write_text(json.dumps(baseline,indent=2)+'\n')
        print(f'Baseline saved: {args.baseline}');return
    baseline=json.loads(args.baseline.read_text());rows={};libraries={}
    def dependencies(metadata):
        for dep in (metadata.get('module') or {}).get('dependencies',[]):
            if dep['name'] in libraries:continue
            path=args.build/'lib'/dep['name']
            size,manifest=package(path);libraries[dep['name']]=size;dependencies(manifest)
    print('Application       Before MB     After MB     Reduction')
    for name in args.names:
        path=args.build/'apps'/f'{name}.fxe'
        if not path.exists():path=args.build/f'{name}.fxe'
        size,manifest=package(path);dependencies(manifest)
        before=baseline[name]['total'];reduction=1-size['total']/before
        rows[name]=dict(before=before,after=size,reduction=reduction)
        print(f"{name:<16} {before/1e6:9.3f}    {size['total']/1e6:9.3f}     {reduction:8.1%}")
    print('\nShared libraries (installed once):')
    for name,size in sorted(libraries.items()):print(f"  {name:<22} {size['total']/1e6:8.3f} MB")
    before=sum(row['before'] for row in rows.values())
    after=sum(row['after']['total'] for row in rows.values())+sum(size['total'] for size in libraries.values())
    print(f'\nComplete selected installation: {before/1e6:.3f} -> {after/1e6:.3f} MB ({1-after/before:.1%} smaller)')
    print('Host octets; guest C16 disk storage uses two physical octets per package octet.')
    result=dict(applications=rows,libraries=libraries,total_before=before,total_after=after)
    if args.json:args.json.write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':main()
