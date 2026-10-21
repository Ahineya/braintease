#!/usr/bin/env python3
"""Prove output arrives before EOF, bounded live data, and real Unix composition."""
from pathlib import Path
import argparse,json,os,select,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/jpipe/host')
a=p.parse_args();b=a.build.resolve()
def live(name,args,first,expected,tail=b'',last=b''):
    child=subprocess.Popen([str(b/name),*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        child.stdin.write(first);child.stdin.flush()
        assert select.select([child.stdout],[],[],5)[0],name+' buffered until EOF'
        assert child.stdout.readline()==expected
        output,error=child.communicate(tail,timeout=10)
        assert child.returncode==0 and output==last,(name,child.returncode,output,error)
    finally:
        if child.poll() is None:child.kill();child.wait()
live('jmap',['x=>x+1'],b'1\n',b'2\n',b'2\n',b'3\n')
live('jobj',[],b'[{"x":1},',b'{"x":1}\n',b'{"x":2}]',b'{"x":2}\n')
live('jpipe',['--input=json','--map','x=>x*2','--filter','x=>x>0'],b'[1,',b'2\n',b'2]',b'4\n')
# Earlier stages remain streaming even when a later sort/reduce must wait.
for args in (['--sort','(a,b)=>a-b'],['--reduce','(a,b)=>a+b','--init','0']):
    child=subprocess.Popen([str(b/'jpipe'),*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        child.stdin.write(b'2\n1\n');child.stdin.flush()
        assert not select.select([child.stdout],[],[],.1)[0]
        output,error=child.communicate(timeout=10)
        assert child.returncode==0 and output==(b'1\n2\n' if args[0]=='--sort' else b'3\n'),error
    finally:
        if child.poll() is None:child.kill();child.wait()
# A document far larger than the engine budget: only array elements and a
# scalar accumulator are retained. Full-file JSON.parse would exceed the cap.
data=json.dumps(list(range(100000))).encode()
r=subprocess.run([str(b/'jpipe'),'--memory','524288','--input=json','--reduce','(a,b)=>a+b','--init','0'],input=data,capture_output=True,timeout=20)
assert r.returncode==0 and r.stdout==b'4999950000\n',(r.returncode,r.stdout,r.stderr)
# Independent processes exchange exactly the same wire values as jpipe.
commands=[['jlines'],['jmap','s=>s.toLowerCase()'],['jfilter','s=>s.includes("test")'],['jreduce','--init','""','(a,s,i)=>a+(i?", ":"")+s'],['jtext']]
children=[]
with tempfile.TemporaryFile() as source:
    source.write(b'TEST-one\nskip\nTest-two\n');source.seek(0);previous=source
    try:
        for args in commands:
            child=subprocess.Popen([str(b/args[0]),*args[1:]],stdin=previous,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if previous is not source:previous.close()
            previous=child.stdout;children.append(child)
        result=children[-1].stdout.read()
        assert result==b'test-one, test-two\n',result
        for child in children:assert child.wait(timeout=10)==0,child.stderr.read()
    finally:
        for child in children:
            if child.poll() is None:child.kill();child.wait()
# Closing the downstream end must cancel a producer instead of hanging.
child=subprocess.Popen([str(b/'jpipe'),'--input=text'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
child.stdout.close()
_,err=child.communicate(b'one\n',timeout=5)
assert child.returncode in (-13,141) and not err,(child.returncode,err)
print('JPIPE_STREAMING_OK: pre-EOF output, incremental root arrays, barrier semantics, bounded memory, real five-process pipeline and broken-pipe cancellation')
