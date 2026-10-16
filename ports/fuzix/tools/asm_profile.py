#!/usr/bin/env python3
"""Select the MMU profile in the two hand-written entry files."""
import re
import sys
from pathlib import Path
source, destination, enabled = sys.argv[1:]
active = [True]
lines = []
for line in Path(source).read_text().splitlines():
    directive = line.strip()
    if directive == '#if RIPPLE_MMU':
        active.append(active[-1] and enabled == '1')
    elif directive == '#else':
        active[-1] = active[-2] and not active[-1]
    elif directive == '#endif':
        active.pop()
    elif active[-1]:
        lines.append(re.sub(r'\bUSER_SB\b', '31' if enabled == '1' else '9', line))
assert len(active) == 1
Path(destination).write_text('\n'.join(lines) + '\n')
