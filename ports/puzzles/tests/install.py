#!/usr/bin/env python3
"""Keep optional puzzles out of normal images, even with old build artifacts."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]


def main():
    games=json.loads((ROOT/'ports/puzzles/catalog.json').read_text())
    names=['native-hello','cat','ls','cp','sh','nano-X','puzzles',*[game['id'] for game in games]]
    with tempfile.TemporaryDirectory(prefix='puzzles-install-') as directory:
        work=Path(directory);apps=work/'apps';apps.mkdir();disk=work/'root.img'
        for name in names:(apps/f'{name}.fxe').symlink_to(ROOT/'fuse-x/build/apps'/f'{name}.fxe')
        subprocess.run(['node',str(ROOT/'fuse-x/image.mjs'),'create',str(disk),'--apps',str(apps)],check=True)
        script=r'''
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {retireInstalledPuzzles} from './ports/puzzles/files.mjs';
const [disk,apps]=process.argv.slice(1), r=rootReader(openImage(disk));
if(r.checked.issues.length)throw Error(r.checked.issues.join('\n'));
const games=JSON.parse(fs.readFileSync('ports/puzzles/catalog.json','utf8'));
for(const name of ['native-hello','cat','ls','cp','sh','nano-X']) {
  const node=r.lookup('/bin/'+name);
  if(node.size!==fs.statSync(apps+'/'+name+'.fxe').size)throw Error('wrong executable '+name);
}
function absent(path) {
  try {r.lookup(path);} catch(error) {
    if(error.message===`missing ${path}`)return;
    throw error;
  }
  throw Error('unexpected optional installation: '+path);
}
for(const name of ['puzzles',...games.map(game=>game.id)])absent('/bin/'+name);
const text=p=>String.fromCharCode(...r.read(p));
absent('/usr/share/doc/puzzles');
if(text('/etc/nxlaunch.cnf').includes('/bin/puzzles'))throw Error('unexpected launcher entry');
console.log('Normal image: core apps installed, all 40 optional games excluded, filesystem clean');
const fixtures=apps+'/retirement';fs.mkdirSync(fixtures);
const old=Buffer.from('former managed game'), custom=Buffer.from('user replacement');
for(const name of ['puzzles','mines','net'])fs.writeFileSync(fixtures+'/'+name+'.fxe',old);
const before=installRoot(openImage(disk),[
  {name:'/bin/puzzles',data:old},{name:'/bin/mines',data:old},
  {name:'/bin/net',data:custom},{name:'/tmp/keep-mines',link:'/bin/mines'},
  {name:'/root/.puzzles/mines.save',data:custom},
]);
const after=installRoot(before,retireInstalledPuzzles(fixtures)), cleaned=rootReader(after);
for(const name of ['puzzles','mines']) {
  try {cleaned.lookup('/bin/'+name);throw Error('retirement failed '+name);}
  catch(error) {if(error.message!=='missing /bin/'+name)throw error;}
}
for(const [name,data] of [['/bin/net',custom],['/tmp/keep-mines',old],['/root/.puzzles/mines.save',custom]])
  if(Buffer.from(cleaned.read(name)).compare(data))throw Error('cleanup changed '+name);
if(cleaned.checked.issues.length)throw Error(cleaned.checked.issues.join('\n'));
if(installRoot(after,retireInstalledPuzzles(fixtures))!==after)throw Error('cleanup is not idempotent');
console.log('Retirement: matching copies removed; replacements, hard links and saves preserved');
'''
        subprocess.run(['node','--input-type=module','-e',script,str(disk),str(apps)],cwd=ROOT,check=True)


if __name__=='__main__':main()
