#!/usr/bin/env python3
"""Generate the upstream glyph mapping in a C16 guest and pack original tiles."""
from pathlib import Path
import json,re,struct,subprocess,sys
PORT=Path(__file__).resolve().parent; ROOT=PORT.parents[1]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def pack_tiles():
    pixels=bytearray();counts=[]
    for name in ['monsters','objects','other']:
        source=(PORT/'upstream/win/share'/(name+'.txt')).read_text()
        palette={symbol:tuple(map(int,channels.split(','))) for symbol,channels in re.findall(r'^([.A-Za-z0-9])\s*=\s*\(([^)]+)\)',source,re.M)}
        tiles=re.findall(r'# tile[^\n]*\n\{\n(.*?)\n\}',source,re.S);counts.append(len(tiles))
        for tile in tiles:
            rows=[row.strip() for row in tile.splitlines()]
            if len(rows)!=16 or any(len(row)!=16 for row in rows): raise RuntimeError('Invalid tile dimensions')
            for row in rows:
                for symbol in row:
                    r,g,b=palette[symbol]
                    pixels.extend(struct.pack('<H',((r>>3)<<11)|((g>>2)<<5)|(b>>3)))
    if counts!=[394,456,232]: raise RuntimeError('Incomplete NetHack tiles')
    return b'NXT1\n'+struct.pack('<H',sum(counts))+pixels

def main():
    work=PORT/'build/nx-assets';work.mkdir(parents=True,exist_ok=True)
    subprocess.run([sys.executable,str(PORT/'build.py'),'fuse-x','--group','tilemap'],check=True,cwd=ROOT)
    manifest=[dict(name='/nh/util/tilemap',path=str(PORT/'build/fuse-x/tilemap.fxe'),mode=0o100755),dict(name='/nh/src',directory=True)]
    (work/'manifest.json').write_text(json.dumps(manifest))
    subprocess.run(['node',str(PORT/'test_image.mjs'),str(work/'manifest.json'),str(work/'disk.img'),'native',str(ROOT/'fuse-x/build')],check=True,cwd=ROOT)
    guest=Guest(work,'native',{},reuse=True)
    try:
        guest.shell('cd /nh/util')
        result=guest.shell('./tilemap; echo TILE-STATUS-$?',120)
        print(result,flush=True)
        if 'TILE-STATUS-0' not in result: raise RuntimeError('Target tilemap generator failed')
    finally: guest.close()
    subprocess.run(['node','--input-type=module','-e',"""
import fs from 'node:fs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
const [disk,out]=process.argv.slice(1), reader=rootReader(openImage(disk));
if(reader.checked.issues.length)throw Error(reader.checked.issues.join('\\n'));
const cells=reader.read('/nh/src/tile.c');
if(cells.some(c=>c>255))throw Error('non-octet tile map source');
fs.writeFileSync(out,Buffer.from(cells));
""",str(work/'disk.img'),str(work/'tile.c')],check=True,cwd=ROOT)
    mapping=(work/'tile.c').read_text()
    values=list(map(int,re.search(r'glyph2tile\[MAX_GLYPH\] = \{(.*?)\};',mapping,re.S)[1].replace('\n','').strip().rstrip(',').split(',')))
    if len(values)<5000 or any(v<0 or v>=1082 for v in values): raise RuntimeError('Invalid glyph mapping')
    data=pack_tiles();(work/'nxtiles').write_bytes(data)
    (PORT/'build/nethack-data/nxtiles').write_bytes(data)
    (work/'report.json').write_text(json.dumps(dict(tiles=1082,glyphs=len(values),bytes=len(data)),indent=2)+'\n')
    print('Packed 1082 original tiles and '+str(len(values))+' target-generated glyph mappings')

if __name__=='__main__': main()
