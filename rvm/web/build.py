#!/usr/bin/env python3
"""Build a self-contained browser site using a fresh allowlisted guest disk."""
from pathlib import Path
import argparse,gzip,hashlib,json,shutil,subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[2]
SOURCE=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,default=ROOT/'build/rvm-web/site')
p.add_argument('--skip-rust',action='store_true')
a=p.parse_args();destination=a.output.resolve();output=destination
if output==ROOT or ROOT in output.parents and output in [ROOT/'public',SOURCE]:raise SystemExit('choose a dedicated output directory')
if not a.skip_rust:subprocess.run(['cargo','build','-p','rvm','--lib','--release','--target','wasm32-unknown-unknown','--target-dir',str(ROOT/'build/rvm-web/cargo'),'--locked'],cwd=ROOT,check=True)
kernel=ROOT/'build/rvm-web/fuzix'
subprocess.run(['make','-C',str(ROOT/'ports/fuzix'),'-j3',f'BUILD={kernel}','kernel','bios'],check=True)
if not (SOURCE/'node_modules/@xterm/xterm/lib/xterm.js').is_file():subprocess.run(['npm','ci','--ignore-scripts','--no-audit','--no-fund'],cwd=SOURCE,check=True)
destination.parent.mkdir(parents=True,exist_ok=True)
staging=tempfile.TemporaryDirectory(prefix=".rvm-web-build-",dir=destination.parent)
output=Path(staging.name)/"site"
output.mkdir()
# Only explicit copies reach the HTTP root. No normal disk or host directory mount.
for name in ['index.html','page.mjs','worker.mjs','disk.mjs','style.css']:
 shutil.copyfile(SOURCE/name,output/name)
shutil.copyfile(ROOT/'build/rvm-web/cargo/wasm32-unknown-unknown/release/rvm.wasm',output/'rvm.wasm')
vendor=output/'vendor';vendor.mkdir(exist_ok=True)
package=SOURCE/'node_modules/@xterm/xterm'
for source,name in [('lib/xterm.js','xterm.js'),('css/xterm.css','xterm.css'),('LICENSE','xterm.LICENSE')]:shutil.copyfile(package/source,vendor/name)
with tempfile.TemporaryDirectory(prefix='rvm-web-seed-') as temporary:
 seed=Path(temporary)/'seed.img'
 subprocess.run(['node',str(SOURCE/'image.mjs'),str(seed),str(kernel)],cwd=ROOT,check=True)
 # Validate the pristine seed before packaging, and retain its public provenance.
 subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(seed)],check=True)
 with seed.open('rb') as src,(output/'disk.img.gz').open('wb') as raw:
  with gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0,compresslevel=6) as dst:shutil.copyfileobj(src,dst)
 shutil.copyfile(str(seed)+'.manifest.json',output/'seed-manifest.json')
meta={name:dict(bytes=(output/name).stat().st_size,sha256=hashlib.sha256((output/name).read_bytes()).hexdigest()) for name in ['rvm.wasm','disk.img.gz','boot.bin'] if (output/name).is_file()}
shutil.copyfile(kernel/'bios.bin',output/'boot.bin')
meta['boot.bin']=dict(bytes=(output/'boot.bin').stat().st_size,sha256=hashlib.sha256((output/'boot.bin').read_bytes()).hexdigest())
(output/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
# Publish complete files atomically, so a live server never reads partial gzip.
for source in output.rglob('*'):
 if source.is_file():
  target=destination/source.relative_to(output);target.parent.mkdir(parents=True,exist_ok=True)
  temporary=target.with_name(target.name+'.new')
  shutil.copyfile(source,temporary);os.replace(temporary,target)
staging.cleanup()
print('Built browser site:',destination)
