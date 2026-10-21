#!/usr/bin/env python3
"""Record the selected kernel heap profile without invalidating unchanged builds."""
import argparse
import os
from pathlib import Path
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--profile',choices=('hosted','bounded'),required=True)
a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
stamp=a.build/'heap-profile';value=a.profile+'\n'
if not stamp.exists() or stamp.read_text()!=value:
    with tempfile.NamedTemporaryFile(mode='w',dir=a.build,delete=False) as f:
        f.write(value);temporary=f.name
    os.replace(temporary,stamp)
print(stamp)
