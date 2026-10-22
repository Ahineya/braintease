#!/usr/bin/env python3
"""Compile and exercise the optional application-registration client."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='gx-application-') as directory:
    binary = Path(directory) / 'application'
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    # Match the other host harnesses when the default SDK uses a TAPI format
    # newer than the installed linker; an explicit SDK choice takes precedence.
    compatible_sdk = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
    if sys.platform == 'darwin' and not os.environ.get('SDKROOT') and compatible_sdk.exists():
        compiler += ['-isysroot', str(compatible_sdk)]
    subprocess.run([*compiler, '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(ROOT / 'ports/graphics/include'),
                    str(ROOT / 'ports/graphics/tests/application.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
