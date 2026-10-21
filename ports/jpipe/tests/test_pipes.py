#!/usr/bin/env python3
"""Real guest pipelines with a producer/consumer handshake proving pre-EOF flow."""
from pathlib import Path
import argparse,sys,tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('native','ripple'),default='native')
a=p.parse_args();target=a.target
build=ROOT/('fuse-x/build' if target=='native' else 'build/fuzix')
ext='fxe' if target=='native' else 'rxe'
apps={name:build/(f'apps/{name}.fxe' if target=='native' else f'{name}.rxe') for name in ('sleep','test')}
apps['jpipe']=ROOT/'build/jpipe'/target/('jpipe.'+ext)
# The producer cannot finish its JSON array until the downstream map has handled
# the first item and logged ACK. Whole-document or whole-stage buffering deadlocks.
script='''(echo '[1,'; while test ! -s /tmp/ack; do sleep 1; done; echo '2]') | jobj | jmap 'x=>{console.error("ACK");return String(x+1);}' 2>/tmp/ack | jtext > /tmp/result
 echo PIPE_STATUS:$?
 cat /tmp/result
'''
files=[dict(name='/bin/'+name,symlink='jpipe') for name in ('jobj','jmap','jtext')]
files.append(dict(name='/tmp/pipeline',text=script))
with tempfile.TemporaryDirectory(prefix='jpipe-pipes-',dir='/tmp') as directory:
    g=Guest(Path(directory),target,apps,files,blocks=65536)
    try:
        text=g.shell('sh /tmp/pipeline',seconds=240).split('\n',1)[1]
        assert 'PIPE_STATUS:0\n2\n3\n' in text,text
        assert g.shell('cat /tmp/ack').count('ACK')==2
        print('JPIPE_PIPES_OK '+target+': three simultaneous engines, real pipe backpressure and producer/consumer pre-EOF handshake',flush=True)
    finally:
        (ROOT/'build/jpipe'/(target+'-pipes.log')).write_bytes(g.log);g.close()
