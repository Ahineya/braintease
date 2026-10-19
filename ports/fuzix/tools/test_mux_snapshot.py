#!/usr/bin/env python3
"""Fast snapshot/tree checks; no PTYs, signals or guest commands are executed."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='mux-snapshot-') as temporary:
    work = Path(temporary)
    command = ['/usr/bin/clang' if sys.platform == 'darwin' else 'cc', '-Wall', '-Wextra',
               '-Wno-unused-function', str(ROOT / 'ports/fuzix/tools/muxsnapshotcheck.c'), '-o', str(work / 'check')]
    if sys.platform == 'darwin':
        command += ['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
    subprocess.run(command, check=True)
    subprocess.run([str(work / 'check'), str(work)], check=True, timeout=10)
