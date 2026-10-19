#!/usr/bin/env python3
"""Independent pixels and protocol-operation counts for retained image drawing."""
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='image-nano-') as name:
    folder=Path(name)
    (folder/'nano-X.h').write_text('#ifndef TEST_NANO_X\n#define TEST_NANO_X\ntypedef unsigned long GR_DRAW_ID,GR_GC_ID;\n#endif\n')
    command=['cc','-std=c99','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
        '-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(folder),'-I'+str(ROOT/'ports/fusede/include'),
        str(ROOT/'libraries/tests/image_nano.c'),str(ROOT/'libraries/image/nano.c'),'-o',str(folder/'check')]
    subprocess.run(command,check=True)
    subprocess.run([folder/'check'],check=True)
