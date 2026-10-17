#!/usr/bin/env python3
"""Enable hosted graphics readiness without changing the concurrent VM port."""
import sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
old = '#ifndef CONFIG_FUSE_X\n        if(major(dev)==4)return ripple_gfx_status();\n#endif'
if source.count(old) != 1:
    raise SystemExit('selectwait graphics dispatch contract changed')
Path(sys.argv[2]).write_text(source.replace(old, '        if(major(dev)==4)return ripple_gfx_status();'))
