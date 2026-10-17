#!/usr/bin/env python3
"""Lynx installation, locking, configuration preservation and installed guest launch."""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from guest import native_image, ripple_image
from probe import PORT, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/fuzix')
    args = parser.parse_args()
    native = args.target == 'native'
    with tempfile.TemporaryDirectory(prefix='lynx-install-', dir='/tmp') as temporary:
        directory = Path(temporary)
        image = directory / 'root.img'
        if native:
            native_image(image, args.kernel.resolve(), blocks=65536)
            command = [str(args.kernel.resolve()), '--run', str(image)]
        else:
            shutil.copytree(PORT / 'fixtures', directory / 'share')
            ripple_image(directory, image, args.build_dir.resolve(), PORT / 'build/lynx.rxe')
            command = [str(ROOT / 'target/release/rvm'), str(args.build_dir.resolve() / 'bios.bin'),
                       '--disk', str(image), '--memory', '16384000', '--net', 'off', '--headless', '--jit', '--timeout', '180']
        install = [sys.executable, str(PORT / 'install.py'), '--target', args.target, '--disk', str(image)]
        # Verify both locking conventions before performing the installation.
        for path in [image, Path(str(image) + '.lock')]:
            with path.open('a+b') as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                rejected = subprocess.run(install, capture_output=True, text=True)
                assert rejected.returncode and 'Disk is in use' in rejected.stderr, rejected
                assert not list(directory.glob('root.img.bak*'))
        subprocess.run(install, check=True)
        backups = list(directory.glob('root.img.bak*'))
        assert len(backups) == 1
        subprocess.run(install, check=True)
        assert list(directory.glob('root.img.bak*')) == backups
        # Modify the guest's configuration, then ensure a repeat install keeps it.
        commands = ('echo "# USER_CONFIG_RETAINED" >> /etc/lynx.cfg\n'
                    'lynx -dump -nolist; echo INSTALLED_STATUS:$?\n'
                    'TERMCAP=/missing lynx; echo TERMINAL_STATUS:$?\nshutdown\n')
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='180')
        if native:
            result = subprocess.run(command, input=commands, text=True, capture_output=True, env=environment, timeout=190)
        else:
            result = subprocess.run([*command, '--input', commands], text=True, capture_output=True, env=environment, timeout=190)
        text = result.stdout.replace('\r', '') + result.stderr
        (PORT / f'build/install-{args.target}.log').write_text(text)
        assert result.returncode == 0 and 'INSTALLED_STATUS:0\n' in text, text
        assert 'TERMINAL_STATUS:1\n' in text and 'cannot initialize terminal' in text, text
        for expected in ['Welcome to Lynx', 'Search Wiby', 'Ctrl-U clears the field', '64 KiB']:
            assert expected in text, text
        subprocess.run(install, check=True)
        # Inspect the retained configuration without another expensive browser load.
        read = 'cat /etc/lynx.cfg\nshutdown\n'
        if native:
            # cat is not needed: use the guest shell's read builtin.
            read = 'while read line; do echo "$line"; done < /etc/lynx.cfg\nshutdown\n'
            checked = subprocess.run(command, input=read, text=True, capture_output=True, env=environment, timeout=30)
        else:
            read = 'while read line; do echo "$line"; done < /etc/lynx.cfg\nshutdown\n'
            checked = subprocess.run([*command, '--input', read], text=True, capture_output=True, env=environment, timeout=50)
        assert checked.returncode == 0 and 'USER_CONFIG_RETAINED' in checked.stdout, checked.stdout + checked.stderr
        assert len(list(directory.glob('root.img.bak*'))) == 1
    print(args.target + ': installed browser, both disk locks, idempotency, defaults and user config passed')


if __name__ == '__main__': main()
