#!/usr/bin/env python3
"""New game, a turn, save, reboot, restore and save again on disposable disks."""
from pathlib import Path
import os,sys,subprocess,json,time,re,shutil,shlex
PORT=Path(__file__).resolve().parent;ROOT=PORT.parents[1]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('target',choices=['native','ripple'])
parser.add_argument('--levels',action='store_true',help='load the Oracle special level in a disposable wizard game')
parser.add_argument('--shared',action='store_true',help='launch through a private native host share')
parser.add_argument('--keys',action='store_true',help='use an arrow to answer a gameplay direction prompt')
parser.add_argument('--home',default='/root',help='guest test home directory')
args=parser.parse_args()
target=args.target
home=args.home
player='wizard' if args.levels else 'Smoke'
username='wizard' if args.levels else 'root'
if not home.startswith('/') or len(home)>200: parser.error('home must be an absolute guest path')
if args.shared and target!='native': parser.error('--shared requires native')
if args.keys and args.levels: parser.error('--keys and --levels are separate tests')
where=PORT/('build/smoke-'+target+('-levels' if args.levels else '-shared' if args.shared else '')+('-keys' if args.keys else ''));where.mkdir(parents=True,exist_ok=True)
files=[dict(name='/bin/nethack',mode=0o100755,path=str(PORT/('build/fuse-x/nethack.fxe' if target=='native' else 'build/fuzix/nethack.rxe'))),
 dict(name='/etc/passwd',mode=0o100644,text=username+':x:0:0:'+username+':'+home+':/bin/sh\n'),
 dict(name='/etc/termcap',mode=0o100644,text='ansi|vt100|xterm|xterm-256color|ripple:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n'),
 dict(name=home+'/.nethackrc',mode=0o100600,text='OPTIONS=color,!news,!autopickup,time,pettype:none\n')]
files += [dict(name='/usr/share/nethack/'+p.name,mode=0o100644,path=str(p)) for p in (PORT/'build/nethack-data').iterdir()]
command='nethack'
if args.shared:
 share=where/'host';share.mkdir(exist_ok=True)
 shutil.copy2(PORT/'build/fuse-x/nethack.fxe',share/'nethack.fxe')
 shutil.copytree(PORT/'build/nethack-data',share/'nethack-data',dirs_exist_ok=True)
 files=[f for f in files if f['name']!='/bin/nethack' and not f['name'].startswith('/usr/share/nethack/')]
 wrapper=where/'kernel-wrapper'
 wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(str(ROOT/'fuse-x/build/fuse-x'))+' \"$@\" --share '+shlex.quote(str(share))+'\n')
 wrapper.chmod(0o755)
 os.environ['FUSE_X_TEST_KERNEL']=str(wrapper)
 command='/mnt/host/nethack.fxe'
(where/'manifest.json').write_text(json.dumps(files))
subprocess.run(['node',str(PORT/'test_image.mjs'),str(where/'manifest.json'),str(where/'disk.img'),target,str(ROOT/('fuse-x/build' if target=='native' else 'build/fuzix'))],check=True,cwd=ROOT)
guest=Guest(where,target,{},reuse=True)

def wait(marker,seconds=180):
 until=time.monotonic()+seconds;more=guest.at
 while time.monotonic()<until:
  guest.pump(); raw=guest.log[guest.at:]
  if marker in raw:
   return guest.read(marker,1)
  if b'Hit space to continue:' in raw or b'--More--' in guest.log[more:]:
   more=len(guest.log); os.write(guest.master,b' ')
  if b'Program initialization has failed' in raw or b'dungeon collapses' in raw:
   
   for j in range(200): guest.pump(); time.sleep(.01)
   raise AssertionError(guest.log[-7000:].decode(errors='replace'))
  if guest.process.poll() is not None: break
 raise AssertionError('Missing '+repr(marker)+'\n'+guest.log[-5000:].decode(errors='replace'))

try:
 os.write(guest.master,(command+(' -D' if args.levels else '')+' -u '+player+' -p Valkyrie -r human -@\r').encode())
 print(wait(b'welcome to NetHack'),flush=True)
 if args.levels:
  os.write(guest.master,b'\x16')
  print(wait(b'To what level'),flush=True)
  os.write(guest.master,b'oracle\r')
  screen=wait(b'Dlvl:')+wait(b'T:1')
  depth=re.search(r'Dlvl:(\d+)',screen)
  assert depth and 5<=int(depth[1])<=9,screen
  os.write(guest.master,b'S')
  print(wait(b'Really save'),flush=True)
  os.write(guest.master,b'y')
  print(wait(guest.prompt),flush=True)
  guest.close();guest=None
  subprocess.run(['node',str(PORT/'verify_state.mjs'),str(where/'disk.img'),target,home,player],check=True,cwd=ROOT)
  print('SPECIAL LEVEL PASS '+target,flush=True)
  sys.exit(0)
 if args.keys:
  # Kick consumes a turn whether aimed at floor or a wall, independent of
  # the randomized starting room. Direction prompts use the movement path.
  os.write(guest.master,b'\x04')
  print(wait(b'In what direction'),flush=True)
  os.write(guest.master,b'\x1b[C')
  print(wait(b'T:2'),flush=True)
 turn=b'T:3' if args.keys else b'T:2'
 os.write(guest.master,b'.')
 print(wait(turn),flush=True)
 os.write(guest.master,b'S')
 print(wait(b'Really save'),flush=True)
 os.write(guest.master,b'y')
 print(wait(guest.prompt),flush=True)
 print(guest.shell('cat '+home+'/.nethackrc'),flush=True)
 guest.close();guest=None
 guest=Guest(where,target,{},reuse=True)
 os.write(guest.master,(command+' -u '+player+'\r').encode())
 print(wait(b'Restoring save file'),flush=True)
 print(wait(turn),flush=True)
 os.write(guest.master,b'S')
 print(wait(b'Really save'),flush=True)
 os.write(guest.master,b'y')
 print(wait(guest.prompt),flush=True)
 guest.close();guest=None
 subprocess.run(['node',str(PORT/'verify_state.mjs'),str(where/'disk.img'),target,home,player],check=True,cwd=ROOT)
 print('SMOKE PASS '+target,flush=True)
finally:
 if guest is not None:
  (where/'failure.log').write_bytes(guest.log)
  if guest.process.poll() is None:guest.process.kill();guest.process.wait()
  os.close(guest.master)
