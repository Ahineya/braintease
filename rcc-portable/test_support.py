#!/usr/bin/env python3
"""Compiler-local conversion/stdio support on both guest execution backends."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from guest_build import HERE, FORMATTERS, support
from test_hosted import Guest

ROOT = HERE.parent


def main():
    formatter = support(HERE / 'build/guest-support')
    with tempfile.TemporaryDirectory(prefix='compiler-support-') as temporary:
        work = Path(temporary)
        for osname, suffix in [('fuse-x', 'fxe'), ('fuzix', 'rxe')]:
            output = work / ('support.' + suffix)
            command = [sys.executable, str(HERE / ('fuse_x.py' if osname == 'fuse-x' else 'fuzix.py')),
                       str(HERE / 'tests/guest_libc.c'), str(HERE / 'guest/strtoull.c'),
                       str(HERE / 'src/strtod.c'), str(formatter), '-I', str(HERE / 'guest/include'),
                       '-DRIPPLE_FLOAT_FORMAT', '--float-format', '-o', str(output)]
            if osname == 'fuzix': command += ['--backend', 'portable']
            for name in FORMATTERS: command += ['-D', name + '=rcc_c_' + name]
            subprocess.run(command, check=True)
            directory = work / osname; directory.mkdir()
            manifest = directory / 'files.json'
            manifest.write_text(json.dumps([dict(guest='/support', host=str(output), mode=0o100755)]))
            image = directory / 'root.img'
            if osname == 'fuse-x':
                subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image), '--apps', str(ROOT / 'fuse-x/build/apps')], check=True)
                subprocess.run(['node', str(HERE / 'test_rvm_image.mjs'), 'install-native', str(image), str(directory), str(manifest)], check=True)
                share = directory / 'share'; share.mkdir(); scratch = directory / 'scratch'; scratch.mkdir()
                guest = Guest(ROOT / 'fuse-x/build/fuse-x', image, share, scratch)
                try:
                    guest.prompt()
                    text = guest.command('/support; echo SUPPORT_STATUS:$?', 180)
                    assert '\nSUPPORT_STATUS:0\n' in text and 'portable guest libc checks passed' in text, text
                    guest.child.stdin.write(b'shutdown\n'); guest.child.stdin.flush()
                    guest.child.communicate(timeout=20)
                    assert guest.child.returncode == 0
                finally: guest.close()
            else:
                build = ROOT / 'build/fuzix'
                (directory / 'apps.json').write_text(json.dumps(['init', 'sh', 'echo']))
                for name in ['init.rxe', 'sh.rxe', 'echo.rxe', 'FUZIX.SYS', 'bios.bin']:
                    (directory / name).symlink_to(build / name)
                subprocess.run(['node', str(HERE / 'test_rvm_image.mjs'), 'create', str(image), str(directory), str(manifest)], check=True)
                result = subprocess.run([str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                    '--memory', '16384000', '--headless', '--jit', '--net', 'off', '--timeout', '180',
                    '--input', '/support\necho SUPPORT_STATUS:$?\nshutdown\n'], capture_output=True, text=True, timeout=200)
                text = result.stdout.replace('\r\n', '\n')
                assert result.returncode == 0 and '\nSUPPORT_STATUS:0\n' in text and 'portable guest libc checks passed' in text, text + result.stderr
            print(osname + ': numeric conversion, buffered I/O, pushback, mode changes and single character evaluation passed', flush=True)


if __name__ == '__main__':
    main()
