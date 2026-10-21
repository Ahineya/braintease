"""Build production keyboard/input fixtures with fatal UBSan; no guest image.

Reference versions: Linux v6.12 atkbd.c and OpenBSD pckbd.c 1.51 /
wskbdmap_mfii.c 1.48 (commit dfb3b89c3c8f7be544280afe44a669f9e0012e27).
Exact source links and deliberately unsupported hardware are in keyboard.c.
"""
from pathlib import Path
import argparse
import json
import os
import platform
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    parser.add_argument('--sysroot')
    parser.add_argument('--result', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cc = shutil.which(args.cc)
    if not cc:
        parser.error('C compiler not found: ' + args.cc)
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
             '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all']
    if args.sysroot:
        flags += ['-isysroot', args.sysroot]
    elif platform.system() == 'Darwin':
        # Installed CLT linker and the 27 SDK can differ on developer machines.
        compatible = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
        if compatible.is_dir():
            flags += ['-isysroot', str(compatible)]
    with tempfile.TemporaryDirectory(prefix='riplix-keyboard-') as folder:
        executable = Path(folder) / 'keyboard-test'
        command = [cc, *flags, str(root / 'tests/keyboard.c'),
                   str(root / 'boot/keyboard.c'), '-o', str(executable)]
        subprocess.run(command, check=True)
        run = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
        print(run.stdout, end='')
        # This verifies native compilation too; it does not claim real execution.
        native_command = [cc, '--target=x86_64-unknown-none-elf', '-std=c11',
                          '-Wall', '-Wextra', '-Werror', '-O2', '-ffreestanding',
                          '-fno-stack-protector', '-mno-red-zone', '-c',
                          str(root / 'boot/keyboard.c'), '-o', str(Path(folder) / 'keyboard.o')]
        subprocess.run(native_command, check=True)
        if args.result:
            args.result.parent.mkdir(parents=True, exist_ok=True)
            args.result.write_text(json.dumps(dict(
                status='passed', host=platform.machine(), compiler=cc,
                sanitizer='undefined (fatal)', output=run.stdout.strip(),
                native_compile='x86_64-unknown-none-elf',
                hardware_acceptance='pending; no image installed'), indent=2) + '\n')


if __name__ == '__main__':
    main()
