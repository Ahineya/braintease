#!/usr/bin/env python3
"""Exercise production mux mouse routing without a graphical display."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='mux-mouse-') as directory:
    binary = Path(directory) / 'check'
    command = ['/usr/bin/clang' if sys.platform == 'darwin' else 'cc', '-O1',
               '-Wall', '-Wextra', '-Wno-unused-function', '-fsanitize=undefined',
               str(ROOT / 'ports/fuzix/tools/muxmousecheck.c'),
               str(ROOT / 'ports/graphics/terminal.c'),
               str(ROOT / 'libraries/unicode/unicode.c'), '-o', str(binary)]
    if sys.platform == 'darwin':
        command += ['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
    subprocess.run(command, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=10)
