#!/usr/bin/env python3
"""Run the real adapted Nano-X event queue on the host, without a GUI."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='nanox-events-') as folder:
    build=Path(folder);source=build/'source'
    subprocess.run([sys.executable,str(ROOT/'ports/nanox/adapt.py'),str(source)],check=True)
    flags=[os.environ.get('CC','cc'),'-O1','-ffunction-sections','-fdata-sections',
        '-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565','-DMWCONFIG_FILE="nxr_config.h"','-DNXR_DESKTOP=1',
        '-I'+str(ROOT/'ports/nanox/ripple'),'-I'+str(source/'include'),'-I'+str(source/'nanox')]
    # Keep the actual dequeue/peek functions. The unused device-facing cursor
    # implementation must not collide with the harness's device stub.
    subprocess.run([*flags,'-DGrMoveCursor=unused_device_cursor','-c',
                    str(source/'nanox/srvfunc.c'),'-o',str(build/'srvfunc.o')],check=True)
    command=[*flags,
        str(ROOT/'ports/nanox/tools/events.c'),str(source/'nanox/srvevent.c'),
        str(build/'srvfunc.o'),'-o',str(build/'events')]
    command+=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']
    subprocess.run(command,check=True)
    subprocess.run([str(build/'events')],check=True)
