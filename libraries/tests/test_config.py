#!/usr/bin/env python3
"""Compile and exercise the reusable C configuration APIs with a host compiler."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc', help='compiler command, including optional SDK flags')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ripple-config-') as temporary:
    executable = Path(temporary) / 'config-test'
    subprocess.run([*shlex.split(args.cc), '-std=c99', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'libraries/tests/config.c'), str(ROOT / 'libraries/ini/ini.c'),
                    str(ROOT / 'libraries/tui-theme/tui_theme.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable), temporary], check=True)
