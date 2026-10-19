#!/usr/bin/env python3
"""Run NetSurf dependency checks on disposable real Riplix and Fuse-X disks."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest
sys.path.insert(0, str(ROOT / 'ports/fuzix/tools'))
from test_symlinks import check_disk


class AuditGuest(Guest):
    def __init__(self, command, system, log_path):
        super().__init__(command, system)
        self.log_path = log_path

    def pump(self):
        size = len(self.log)
        super().pump()
        if len(self.log) != size:
            self.log_path.write_bytes(self.log)


def image(system, build, apps, folder, extra=()):
    files = [{'name': '/init', 'data': 'FXN1:init:1\n'}] if system == 'fuse-x' else [
        {'name': '/init', 'host': str(build / 'init.rxe')}]
    files += [{'name': '/bin/' + name, 'host': str(path)} for name, path in apps.items()]
    files += [{'name': '/dev/tty', 'mode': 0o20666, 'device': 0x200}]
    files += list(extra)
    # Native fixtures must carry the libraries imported by current app builds.
    # Installing a shared browser does not make the shell's libc available.
    if system == 'fuse-x':
        present={f['name'] for f in files}
        files += [dict(name='/lib/fuse-x/'+p.name,host=str(p),mode=0o100644)
                  for p in sorted((build/'lib').glob('*.fxl.[0-9]'))
                  if '/lib/fuse-x/'+p.name not in present]
    manifest = folder / 'files.json'
    manifest.write_text(json.dumps(files))
    disk = folder / 'disk.img'
    # Root files store each executable octet in a C16 cell. Size the private
    # image from the actual linked packages as the dependency graph grows.
    script = r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
import {ptyDevices} from './ports/fuzix/tools/pty.mjs';
import {netsurfFiles} from './ports/netsurf/files.mjs';
import {tlsFiles} from './ports/tls/files.mjs';
import {unicodeFiles} from './libraries/unicode/files.mjs';
const [manifest,disk,build,native]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest));
for(const file of files){
  if(file.host){file.data=fs.readFileSync(file.host);delete file.host;}
  else if(file.data)file.data=Buffer.from(file.data);
}
files.push(...ptyDevices());
const payload=files.reduce((sum,file)=>sum+(file.data?.length||0),0);
// The GUI installer adds these after creating this base image. Reserve their
// actual C16 storage too; the font strikes alone exceed the old 4 MiB margin.
const installPayload=files.some(file=>file.name==='/bin/netsurf')?
  [...netsurfFiles(),...tlsFiles(),...unicodeFiles()].reduce((sum,file)=>sum+file.data.length,0):0;
const blocks=Math.max(32768,Math.ceil((2*(payload+installPayload)+4*1024*1024)/(2048*1024))*1024);
// V3 has 32-bit block references. Keep private fixtures within 256 MiB,
// rather than retaining V2's obsolete 65,535-block ceiling.
if(blocks>131072)throw new Error('NetSurf acceptance files exceed the private test disk budget');
const root=rootImage(files,blocks,256);
const sectors=Math.ceil((root.length+16*1024*1024)/(512*8192))*8192;
const image=native==='1'?root:fat16Image([
  {name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},
  {name:'ROOTFS.IMG',data:root}
],{totalSectors:sectors,sectorsPerFat:256,sectorsPerCluster:8});
atomicWrite(disk,image);
'''
    subprocess.run(['node', '--input-type=module', '-e', script, str(manifest),
                    str(disk), str(build), str(int(system == 'fuse-x'))], cwd=ROOT, check=True)
    return disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/netsurf')
    parser.add_argument('--os', choices=('fuzix', 'fuse-x', 'both'), default='both')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--fuzix-shell', type=Path)
    parser.add_argument('--native-shell', type=Path)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--core', action='store_true', help='Exercise browser layout/redraw instead of dependency checks')
    args = parser.parse_args()
    results = {}
    for system in (('fuse-x', 'fuzix') if args.os == 'both' else (args.os,)):
        native = system == 'fuse-x'
        build = (args.native_build if native else args.fuzix_build).resolve()
        sdk = args.build.resolve() / (('sdk-native' if native else 'sdk-ripple') + ('-float' if args.core else ''))
        shell = (args.native_shell if native else args.fuzix_shell) or sdk / ('apps/sh.fxe' if native else 'sh.rxe')
        shell = shell.resolve()
        name = 'netsurfcorecheck' if args.core else 'netsurfcheck'
        program = args.build.resolve() / (name + ('.fxe' if native else '.rxe'))
        if not args.skip_build:
            args.build.mkdir(parents=True, exist_ok=True)
            with (args.build / (system + '-build.log')).open('w') as log:
                subprocess.run([sys.executable, str(ROOT / 'ports/netsurf/app.py'), '--build', str(args.build.resolve()),
                    '--sdk', str(sdk), '--target', 'native' if native else 'ripple', '-o', str(program),
                    *(['--core', '--source', str(ROOT / 'ports/netsurf/tests/core.c')] if args.core else [])],
                    stdout=log, stderr=log, check=True)
                command = [sys.executable, str(ROOT / 'fuse-x/apps.py'), 'sh', 'echo', '--build', str(sdk)] if native else [
                    'make', '-s', '-C', str(ROOT / 'ports/fuzix'), 'BUILD=' + str(sdk), '-j3',
                    str(sdk / 'sh.rxe'), str(sdk / 'echo.rxe')]
                subprocess.run(command, stdout=log, stderr=log, check=True)
        apps = {'sh': shell, name: program,
                'echo': (build if args.skip_build else sdk) / ('apps/echo.fxe' if native else 'echo.rxe')}
        with tempfile.TemporaryDirectory(prefix='netsurf-guest-') as temporary:
            disk = image(system, build, apps, Path(temporary))
            command = [build / 'fuse-x', '--run', disk] if native else [
                ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', disk,
                '--memory', '16384000', '--net', 'off', '--jit', '--timeout', '600']
            log_path = args.build / (system + ('-core' if args.core else '') + '.log')
            guest = AuditGuest(command, system, log_path)
            try:
                guest.raw(guest.prompt)
                ready = guest.shell('echo NETSURF_READY', seconds=20)
                assert b'NETSURF_READY' in ready, ready
                output = guest.shell(name + '; echo NETSURF_STATUS:$?', seconds=180)
                text = output.decode(errors='replace')
                results[system] = text
                if args.core:
                    assert b'NETSURF_CORE checks=' in output and b'failures=0' in output and b'FAIL ' not in output, text
                else:
                    assert b'NETSURF_OK' in output and b'NETSURF_FAILED' not in output, text
                assert b'NETSURF_STATUS:0' in output, text
                guest.send(b'shutdown\r')
                guest.process.wait(timeout=30)
                assert guest.process.returncode == 0
            finally:
                log_path.write_bytes(guest.log)
                guest.close()
            check_disk(disk, native)
        print(system + ': ' + text.strip(), flush=True)
    (args.build / ('core-guest-results.json' if args.core else 'guest-results.json')).write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
