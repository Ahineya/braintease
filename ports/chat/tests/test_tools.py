#!/usr/bin/env python3
"""Fast offline tool checks; test files and subprocesses stay in a temp folder."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='chat-tools-') as temporary:
    work = Path(temporary)
    (work / 'state').mkdir(mode=0o700)
    (work / 'workspace').mkdir()
    compiler = os.environ.get('CC', '/usr/bin/clang' if sys.platform == 'darwin' else 'cc')
    command = [compiler, '-Wall', '-Wextra', '-I' + str(PORT), str(PORT / 'tests/toolscheck.c'),
               *[str(PORT / (name + '.c')) for name in ('tools', 'tool_history', 'store', 'json')], '-o', str(work / 'check')]
    if sys.platform == 'darwin':
        command += ['-fuse-ld=/Library/Developer/CommandLineTools/usr/bin/ld']
    subprocess.run(command, check=True)
    subprocess.run([str(work / 'check'), str(work / 'state'), str(work / 'workspace')], check=True, timeout=20)
