#!/usr/bin/env python3
"""Verify normal app image creation/upgrades and boot the installed browser."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'lynx'))
from probe import PORT, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    native = args.target == 'native'
    build = (args.build_dir or ROOT / ('fuse-x/build' if native else 'build/fuzix')).resolve()
    package = build / ('apps/lynx.fxe' if native else 'lynx.rxe')
    with tempfile.TemporaryDirectory(prefix='lynx-apps-', dir='/tmp') as temporary:
        directory = Path(temporary)
        stage = directory / 'apps'
        stage.mkdir()
        names = ['sh', 'echo', 'lynx'] if native else ['init', 'sh', 'echo', 'lynx']
        for name in names + (['native-hello'] if native else []):
            destination = stage / (name + ('.fxe' if native else '.rxe'))
            source = build / ('apps/' + name + '.fxe' if native else name + '.rxe')
            if name == 'lynx':
                shutil.copy2(source, destination)
                package = destination
            else:
                destination.symlink_to(source)
        if not native:
            (stage / 'FUZIX.SYS').symlink_to(build / 'FUZIX.SYS')

        def inventory(lynx):
            selected = [n for n in names if lynx or n != 'lynx']
            destination = directory / 'application-report.json' if native else stage / 'apps.json'
            destination.write_text(json.dumps([{'name': n, 'status': 'built'} for n in selected] if native else selected))

        def image_command(image, create=False):
            if native:
                return ['node', str(ROOT / 'fuse-x/image.mjs'), 'create' if create else 'install',
                        str(image), '--apps', str(stage)]
            return ['node', str(ROOT / 'ports/fuzix/tools/disk.mjs'), str(stage), str(image), '--install']

        def inspect(image, customize=False):
            # Use the canonical reader to compare the whole executable, not
            # just its magic, and to check data/configuration independently.
            script = '''
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
const [filename, target, packagePath, customize] = process.argv.slice(1);
const outer = openImage(filename);
const root = target === 'native' ? {data:outer,replace:d=>d} : fatFile(outer,'ROOTFS.IMG');
if (customize === 'yes') {
  atomicWrite(filename,root.replace(installRoot(root.data,[
    {name:'/etc/lynx.cfg',mode:0o100644,data:Buffer.from('STARTFILE:file:///usr/share/lynx/start.html\\n# USER_CONFIG_RETAINED\\n')},
    {name:'/note',mode:0o100640,data:Buffer.from('saved session')},
  ])));
} else {
  const reader = rootReader(root.data);
  const text = name => String.fromCharCode(...reader.read(name));
  assert.deepEqual(reader.checked.issues,[]);
  const actual=reader.read('/bin/lynx'), expected=fs.readFileSync(packagePath);
  assert.equal(actual.length,expected.length);
  assert.ok(actual.every((c,i)=>c===expected[i]),'complete target executable installed');
  assert.match(text('/etc/lynx.cfg'),/USER_CONFIG_RETAINED/);
  assert.match(text('/usr/share/lynx/start.html'),/Welcome to Lynx/);
  assert.match(text('/usr/share/lynx/COPYING'),/GNU GENERAL PUBLIC LICENSE/);
  assert.equal(text('/note'),'saved session');
}
'''
            subprocess.run(['node', '--input-type=module', '-e', script, str(image), args.target,
                            str(package), 'yes' if customize else 'no'], cwd=ROOT, check=True)

        # Exercise both first launch and an upgrade from a root without Lynx.
        for fresh in (True, False):
            image = directory / ('fresh.img' if fresh else 'upgrade.img')
            inventory(fresh)
            subprocess.run(image_command(image, create=True), check=True)
            inspect(image, customize=True)
            inventory(True)
            subprocess.run(image_command(image), check=True)
            inspect(image)
            before = image.stat().st_mtime_ns
            subprocess.run(image_command(image), check=True)
            assert image.stat().st_mtime_ns == before, 'unchanged app install rewrote the disk'
            if fresh:
                for old in directory.glob('fresh.img*'):
                    old.unlink()
        commands = 'lynx -dump -nolist; echo NORMAL_APP_STATUS:$?\nshutdown\n'
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='180')
        if native:
            command = [str(build / 'fuse-x'), '--run', str(image)]
            result = subprocess.run(command, input=commands, text=True, capture_output=True, env=environment, timeout=190)
        else:
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                       '--memory', '16384000', '--net', 'off', '--headless', '--jit', '--timeout', '180', '--input', commands]
            result = subprocess.run(command, text=True, capture_output=True, env=environment, timeout=190)
        output = result.stdout.replace('\r', '') + result.stderr
        (PORT / f'build/apps-{args.target}.log').write_text(output)
        assert result.returncode == 0 and 'NORMAL_APP_STATUS:0\n' in output, output
        assert 'Welcome to Lynx' in output and 'Search Wiby' in output, output
    print(args.target + ': normal app create/upgrade, config preservation, idempotency and installed browser passed')


if __name__ == '__main__':
    main()
