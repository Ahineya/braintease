#!/usr/bin/env python3
"""Exercise the shared application catalog against real host fork/exec/pipes."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    with tempfile.TemporaryDirectory(prefix='graphite-catalog-') as folder:
        work = Path(folder)
        binary = work / 'catalog-check'
        compiler = os.environ.get('CC')
        environment = os.environ.copy()
        if sys.platform == 'darwin':
            # Resolve compiler and SDK from the same active developer toolchain.
            # /usr/bin/cc can point at a different Xcode installation.
            environment.setdefault('SDKROOT', subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip())
            if not compiler:
                sdk_compiler = Path(environment['SDKROOT']).parent.parent / 'usr/bin/clang'
                compiler = str(sdk_compiler) if sdk_compiler.is_file() else subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
        subprocess.run([compiler or 'cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'ports/lvgl/shell-catalog-check.c'),
                        str(ROOT / 'ports/lvgl/shell-catalog.c'),
                        str(ROOT / 'libraries/ini/ini.c'), '-o', str(binary)], check=True, env=environment)
        subprocess.run([str(binary), str(work), str(binary), str(ROOT / 'ports/fusede/catalog')], check=True)

if __name__ == '__main__':
    main()
