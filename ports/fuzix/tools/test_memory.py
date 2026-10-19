#!/usr/bin/env python3
"""Check real guest memory/string routines and fork COW on private disks."""
import argparse
import json
from pathlib import Path
import tempfile
from bench_foundations import build_probe
from test_symlinks import run, check_disk, Guest

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT/'ports/fuzix'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os', choices=('both', 'fuzix', 'fuse-x'), default='both')
    p.add_argument('--build', type=Path, default=ROOT/'build/fuzix')
    p.add_argument('--native-build', type=Path, default=ROOT/'fuse-x/build')
    p.add_argument('--output', type=Path, default=ROOT/'build/banked-memory/guests')
    p.add_argument('--skip-build', action='store_true')
    a = p.parse_args(); a.output = a.output.resolve(); a.output.mkdir(parents=True, exist_ok=True)
    for system in (['fuse-x', 'fuzix'] if a.os == 'both' else [a.os]):
        native = system == 'fuse-x'
        build = (a.native_build if native else a.build).resolve()
        if not a.skip_build:
            if native:
                run(['make', '-C', ROOT/'fuse-x', '-j4', 'BUILD='+str(build), build/'fuse-x'])
                run(['python3', ROOT/'fuse-x/apps.py', 'sh', 'echo', '--build', build])
            else:
                run(['make', '-C', PORT, '-j4', 'BUILD='+str(build), 'kernel', 'bios', 'rfl-sdk',
                     *[build/(n+'.rxe') for n in ['init', 'sh', 'echo']]])
        probe = a.output/('memorycheck.'+('fxe' if native else 'rxe'))
        build_probe(build, native, probe, PORT/'tools/memorycheck.c')
        with tempfile.TemporaryDirectory(prefix='banked-memory-') as temporary:
            work = Path(temporary); image = work/'disk.img'
            files = [dict(name='/init', data='FXN1:init:1\n') if native else
                     dict(name='/init', host=str(build/'init.rxe'))]
            files += [dict(name='/bin/'+n, host=str(build/'apps'/(n+'.fxe') if native else build/(n+'.rxe')))
                      for n in ['sh', 'echo']]
            files += [dict(name='/bin/memorycheck', host=str(probe)),
                      dict(name='/dev/tty', mode=0o20666, device=0x200)]
            manifest = work/'manifest.json'; manifest.write_text(json.dumps(files))
            run(['node', '--input-type=module', '-e', r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,image,build,native]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest,'utf8'));
for(const f of files)if(f.host){f.data=fs.readFileSync(f.host);delete f.host;}else if(typeof f.data==='string')f.data=Buffer.from(f.data);
const root=rootImage(files,16384,256);
atomicWrite(image,native==='1'?root:fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
''', manifest, image, build, int(native)])
            command = [build/'fuse-x', '--run', image] if native else [
                ROOT/'target/release/rvm', build/'bios.bin', '--disk', image, '--net', 'off',
                '--memory', '16384000', '--timeout', '240', '--report', a.output/'rvm.json']
            guest = Guest(command, system)
            try:
                guest.raw(guest.prompt)
                output = guest.shell('memorycheck; echo MEMORY_STATUS:$?', seconds=180)
                assert b'\nYYYYYYYY\n' in output and b'\nMEMORY_OK\n' in output and b'\nMEMORY_STATUS:0\n' in output, output
                guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
                assert guest.process.returncode == 0
            finally:
                (a.output/(system+'.log')).write_bytes(guest.log); guest.close()
            check_disk(image, native)
        print(system+': wide counts, unequal bank spans, full cells, overlap, zero count and fork COW passed', flush=True)


if __name__ == '__main__':
    main()
