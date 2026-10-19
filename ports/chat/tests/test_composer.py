#!/usr/bin/env python3
"""Offline composer and path-completion checks using the shared Unicode engine."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
with tempfile.TemporaryDirectory(prefix='chat-composer-') as temporary:
    work = Path(temporary)
    (work / 'workspace').mkdir()
    compiler = os.environ.get('CC', '/usr/bin/clang' if sys.platform == 'darwin' else 'cc')
    command = [compiler, '-Wall', '-Wextra', '-idirafter', str(ROOT / 'runtime/include'),
               str(PORT / 'tests/composercheck.c'), str(PORT / 'composer.c'), str(PORT / 'mentions.c'),
               str(PORT / 'json.c'), str(ROOT / 'libraries/unicode/unicode.c'), '-o', str(work / 'check')]
    if sys.platform == 'darwin':
        command += ['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
    subprocess.run(command, check=True)
    subprocess.run([str(work / 'check'), str(work / 'workspace')], check=True, timeout=10)
