#!/usr/bin/env python3
"""Exercise host entropy through actual guest kernels on private small disks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    build = (args.native_build if args.native else args.build).resolve()
    with tempfile.TemporaryDirectory(prefix='fuzix-entropy-') as temporary:
        image = Path(temporary) / 'root.img'
        builder = r"""
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
const [build,disk,native]=process.argv.slice(1);
const app=name=>fs.readFileSync(`${build}/${native==='1'?'apps/'+name+'.fxe':name+'.rxe'}`);
const files=[{name:'/init',mode:0o100755,data:native==='1'?Buffer.from('FXN1:init:1\n'):app('init')},
 ...['sh','echo','entropycheck'].map(n=>({name:'/bin/'+n,mode:0o100755,data:app(n)})),
 {name:'/dev/tty',mode:0o20666,device:0x200},{name:'/dev/console',mode:0o20600,device:0x201},
 {name:'/dev/null',mode:0o20666,device:0x100},{name:'/dev/urandom',mode:0o20444,device:0x104}];
const raw=rootImage(files,8192,256);
if(native==='1')writeImage(disk,raw);else fs.writeFileSync(disk,fat16Image([
 {name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:raw}],
 {totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
"""
        subprocess.run(['node', '--input-type=module', '-e', builder, str(build),
                        str(image), '1' if args.native else '0'], cwd=ROOT, check=True)
        commands = 'entropycheck\necho ENTROPY_STATUS:$?\nentropycheck\necho REPEAT_STATUS:$?\nshutdown\n'
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='90')
        if args.native:
            command = [str(build / 'fuse-x'), '--run', str(image), '--one-shot']
        else:
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                       '--disk', str(image), '--memory', '16384000', '--jit', '--headless',
                       '--net', 'off', '--timeout', '90', '--input', commands]
        result = subprocess.run(command, input=commands if args.native else None,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, timeout=100)
        print(result.stdout, flush=True)
        assert result.returncode == 0 and result.stdout.count('entropycheck passed') == 2, result.stdout
        assert 'ENTROPY_STATUS:0' in result.stdout and 'REPEAT_STATUS:0' in result.stdout, result.stdout
        if args.native:
            import sys
            sys.path.insert(0, str(ROOT / 'fuse-x'))
            from test_support import check_root
            check_root(image)
        else:
            subprocess.run(['python3', str(ROOT / 'ports/fuzix/tools/fsck.py'), str(image)], check=True)
    print('Entropy: OS octets, bank boundaries, short reads, guards, bad pointers, fork/COW and repeated reads passed')


if __name__ == '__main__':
    main()
