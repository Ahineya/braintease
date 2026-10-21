#!/usr/bin/env python3
"""Extract named octet files directly from a clean, stopped guest filesystem."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
EXTRACT_PATHS = r'''
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
const [disk,out,mapping]=process.argv.slice(1), reader=rootReader(openImage(disk));
if(reader.checked.issues.length)throw Error(reader.checked.issues.join('\n'));
const staged=[];
for(const [name,source] of Object.entries(JSON.parse(mapping))) {
  const parts=source.slice(1).split('/');
  for(let n=1;n<parts.length;n++)
    if(reader.lookup('/'+parts.slice(0,n).join('/')).type!==0x4000)
      throw Error('non-directory in extraction path: '+source);
  const node=reader.lookup(source);
  if(node.type!==0x8000)throw Error('not a regular output file: '+source);
  const cells=reader.cells(node);
  if(cells.some(v=>v>255))throw Error('non-octet tool output: '+source);
  staged.push([name,Buffer.from(cells)]);
}
for(const [name,data] of staged)fs.writeFileSync(out+'/'+name,data);
'''


def extract_files(disk, output, mapping):
    """Map flat host output names to absolute guest paths; never copy in-guest."""
    if not mapping:
        raise ValueError('expected guest output paths')
    for name, source in mapping.items():
        if (not isinstance(name, str) or not name or name in ('.', '..')
                or '/' in name or '\\' in name or '\0' in name):
            raise ValueError('expected a flat output filename')
        if (not isinstance(source, str) or not source.startswith('/') or '\0' in source
                or any(p in ('', '.', '..') or len(p) > 30 for p in source[1:].split('/'))):
            raise ValueError('expected a canonical absolute guest path')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['node', '--input-type=module', '-e', EXTRACT_PATHS, str(Path(disk).resolve()),
                    str(output.resolve()), json.dumps(mapping)], cwd=ROOT, check=True)
