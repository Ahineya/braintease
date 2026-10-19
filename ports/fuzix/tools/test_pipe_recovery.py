#!/usr/bin/env python3
"""A live anonymous pipe must leave no disk inode at shutdown; reboot saved files."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    args = parser.parse_args()
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='fuzix-pipe-recovery-') as directory:
        image = Path(directory) / 'test.img'
        create = """
import fs from 'node:fs';
import path from 'node:path';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {Image} from './ports/fuzix/tools/image.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [build,output]=process.argv.slice(1);
const files=['init','sh','echo','cat','sleep'].map(name=>({
  name:name==='init'?'/init':`/bin/${name}`,mode:0o100755,
  data:fs.readFileSync(path.join(build,`${name}.rxe`))}));
files.push({name:'/dev/null',mode:0o20666,device:0x100});
atomicWrite(output,fat16Image([
  {name:'FUZIX.SYS',data:fs.readFileSync(path.join(build,'FUZIX.SYS'))},
  {name:'ROOTFS.IMG',data:rootImage(files,8192,128)}],
  {totalSectors:65536,sectorsPerFat:64,sectorsPerCluster:4,imageFactory:n=>new Image(n)}));
"""
        subprocess.run(['node', '--input-type=module', '-e', create, str(build), str(image)],
                       cwd=ROOT, check=True)

        def boot(commands):
            result = subprocess.run([
                str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                '--disk', str(image), '--memory', '16384000', '--headless',
                '--jit', '--net', 'off', '--input', commands, '--timeout', '40'],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=50)
            assert result.returncode == 0 and 'disk synced; power off.' in result.stdout, result.stdout
            assert 'panic:' not in result.stdout, result.stdout
            return result.stdout.replace('\r', '')

        output = boot("echo persisted > /recovery-note\n"
                      "sh -c 'echo payload; sleep 300' | cat &\nsleep 1\nshutdown\n")
        assert '\npayload\n' in output, output
        checker = ['python3', str(ROOT / 'ports/fuzix/tools/fsck.py'), str(image)]
        # Anonymous pipe rings no longer allocate a disk inode. Historic orphan
        # repair remains covered by disk.test.mjs, including exact backups.
        subprocess.run(checker, check=True)
        output = boot('cat /recovery-note\nshutdown\n')
        assert '\npersisted\n' in output, output
        assert 'warning: mounting dirty' not in output, output
        subprocess.run(checker, check=True)
    print('RVM memory pipes: live pipeline, no orphan inode, saved file, reboot and clean filesystem passed')


if __name__ == '__main__':
    main()
