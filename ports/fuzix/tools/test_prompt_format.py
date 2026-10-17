#!/usr/bin/env python3
"""Compile deterministic formatter checks without a guest image or filesystem build."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PORT=Path(__file__).resolve().parents[1]
environment=os.environ.copy()
flags=[]
if sys.platform=='darwin':
    developer='/Applications/Xcode.app/Contents/Developer'
    if Path(developer).is_dir():environment.setdefault('DEVELOPER_DIR',developer)
    sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],env=environment,text=True).strip()
    flags=['-isysroot',sdk]
with tempfile.TemporaryDirectory(prefix='fuzix-prompt-format-') as folder:
    work=Path(folder)
    # The port has a top-level utsname.h; host libc uses sys/utsname.h.
    (work/'utsname.h').write_text('#include <sys/utsname.h>\n')
    subprocess.run([os.environ.get('CC','cc'),*flags,'-std=c99','-Wall','-Wextra','-Werror',
        '-I'+str(work),'-I'+str(PORT/'ripple'),str(PORT/'ripple/shell-prompt.c'),
        str(PORT/'tools/tests/shell_prompt.c'),'-o',str(work/'check')],check=True,env=environment,timeout=60)
    subprocess.run([str(work/'check')],check=True,timeout=10)
print('Shell prompt: identities, paths, clock, quoting, colors, multiline geometry and bounds passed')
