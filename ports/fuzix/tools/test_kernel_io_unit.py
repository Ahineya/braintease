#!/usr/bin/env python3
"""Run the production pipe, queue and cache helpers against a deterministic model."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[3]
fixture = ROOT / 'ports/fuzix/tools/io-test'
env = os.environ.copy()
sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
if sdk.exists():
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
with tempfile.TemporaryDirectory(prefix='kernel-io-unit-') as directory:
    executable = Path(directory) / 'check'
    subprocess.run(['cc', *(['-isysroot', str(sdk)] if sdk.exists() else []),
                    '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-fsanitize=undefined', '-I'+str(fixture),
                    str(fixture/'check.c'), '-o', str(executable)], check=True, env=env)
    subprocess.run([executable], check=True, timeout=15,
                   env={**env, 'ASAN_OPTIONS': 'detect_leaks=0'})
