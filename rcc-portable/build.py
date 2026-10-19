#!/usr/bin/env python3
"""Build the portable C frontend with the host C compiler."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from adapt import HERE, SOURCES, main as adapt


def main():
    adapt()
    output = HERE / 'build/rcc-c'
    flags = ['-std=c99', '-O2', '-Wall', '-Wextra', '-Wno-switch',
             '-Wno-parentheses', '-I', str(HERE / 'build/frontend')]
    if sys.platform == 'darwin':
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        flags += ['-isysroot', sdk]
    sources = [HERE / f'build/frontend/{name}.c' for name in SOURCES]
    sources += sorted((HERE / 'src').glob('*.c'))
    with tempfile.TemporaryDirectory(prefix='.host-build-', dir=output.parent) as temporary:
        executable = Path(temporary) / 'rcc-c'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *flags,
                        *map(str, sources), '-o', str(executable)], check=True)
        os.replace(executable, output)
    print(output)


if __name__ == '__main__':
    main()
