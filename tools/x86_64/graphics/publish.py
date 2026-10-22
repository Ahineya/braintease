#!/usr/bin/env python3
"""Publish the three prepared GUI development packages, reusing Mini's engine.

Uses the repository's existing guarded signer/index transaction without the
ordinary wrapper's unconditional host compiler rebuild. Signing material never
leaves the Mini. Other application identities are preserved by the publisher.
"""
from pathlib import Path
import hashlib,json,re,subprocess,time
ROOT=Path(__file__).resolve().parents[3];out=ROOT/'build/x86_64-graphics/catalog'
report=json.loads((out/'catalog.json').read_text());packages=report['packages']
assert {(p['name'],p['target']) for p in packages}=={(n,'riplix-x64-c16') for n in ('lvcalc','screenshot','touchpad')}
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=6','mini.local']
def remote(args,**kwargs):return subprocess.run([*ssh,*args],check=True,**kwargs)
before=subprocess.check_output([*ssh,'cat /Volumes/Ahineya/RiplixRepo/public/index.v1'])
(out/'index-before.v1').write_bytes(before)
lines=before.decode().splitlines()
if not lines or lines[0]!='RIPINDEX1':raise RuntimeError('Bad signed repository index')
previous={}
for line in lines[1:]:
    fields=line.split('\t')
    if len(fields)!=7:raise RuntimeError('Bad signed repository record')
    previous[(fields[0],fields[2])]=fields[4]
changed_packages=[p for p in packages if previous.get((p['name'],p['target']))!=p['sha256']]
if not changed_packages:
    print('Repository current for all three identities');raise SystemExit(0)
setup=b'''set -eu
actual=$(/usr/sbin/diskutil info -plist /Volumes/Ahineya | /usr/bin/plutil -extract VolumeUUID raw -)
[ "$actual" = E3129F95-3ACD-4BD1-B261-35D449BB4A1D ]
[ -x "$HOME/.local/share/riplix-repo/pkg-engine" ]
umask 077
mktemp -d /Volumes/Ahineya/RiplixRepo/private/incoming.XXXXXX
'''
incoming=subprocess.check_output([*ssh,'/bin/sh','-s'],input=setup).decode().strip()
if not re.fullmatch(r'/Volumes/Ahineya/RiplixRepo/private/incoming\.[A-Za-z0-9]+',incoming):raise RuntimeError('Bad Mini staging path')
for p in packages:
    if hashlib.sha256(Path(p['archive']).read_bytes()).hexdigest()!=p['sha256']:raise RuntimeError('Package changed')
start=time.monotonic()
subprocess.run(['scp','-C','-o','BatchMode=yes',*[p['archive'] for p in changed_packages],'mini.local:'+incoming+'/'],check=True)
with (ROOT/'tools/packages/repo/publish-remote.sh').open('rb') as script:
    remote(['/bin/sh','-s','--',incoming],stdin=script,stdout=(out/'publish.log').open('wb'),stderr=subprocess.STDOUT)
after=subprocess.check_output([*ssh,'cat /Volumes/Ahineya/RiplixRepo/public/index.v1'])
(out/'index-after.v1').write_bytes(after)
def index(data):
 lines=data.decode().splitlines();assert lines[0]=='RIPINDEX1'
 parsed={}
 for line in lines[1:]:
  fields=line.split('\t');assert len(fields)==7;parsed[(fields[0],fields[2])]=fields
 return parsed
old,new=index(before),index(after)
owned={(p['name'],p['target']) for p in packages}
for p in packages:
 row=new[(p['name'],p['target'])]
 if row[1]!=p['version'] or row[4]!=p['sha256']:raise RuntimeError('Published identity mismatch')
changed=[key for key in old if key not in owned and old[key]!=new.get(key)]
# The standard publisher may prune only unreferenced generated support records.
bad=[key for key in changed if not key[0].startswith(('bundle-','fxlib-'))]
if bad:raise RuntimeError('Unrelated application identities changed: '+str(bad))
receipt={'status':'published','seconds':round(time.monotonic()-start,3),'catalog':str(out/'catalog.json'),'packages':[{'name':p['name'],'version':p['version'],'sha256':p['sha256']} for p in packages],
 'signed_by':'existing Mini-local publisher; private signing key stays on SSD','engine_rebuild':False,'uploaded':[p['name'] for p in changed_packages],'before_packages':len(old),'after_packages':len(new),'unrelated_application_changes':bad,'generated_support_pruned':changed,
 'commands':['pkg update','pkg --adopt-seed add touchpad','pkg --adopt-seed add screenshot','pkg --adopt-seed add lvcalc']}
(out/'publication.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt),flush=True)
