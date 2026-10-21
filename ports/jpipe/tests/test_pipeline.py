#!/usr/bin/env python3
"""Portable semantics on the host or real Fuse-X/RVM kernels, on private disks."""
import argparse,json,os,random,re,select,shlex,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=('host','native','ripple'),default='host')
p.add_argument('--build',type=Path,default=ROOT/'build/jpipe')
a=p.parse_args();out=a.build.resolve()/a.target
aliases=('jlines','jobj','jmap','jfilter','jreduce','jsort','jtext')
results=[]
# Every invocation has its own input file on guests, so shell echo cannot count
# as successful program output. Expected stdout and exit codes are exact.
with tempfile.TemporaryDirectory(prefix='jpipe-test-',dir='/tmp') as temporary:
    work=Path(temporary);guest=None;cases=[]
    def case(label,args,data=b'',expected=b'',code=0,error=None):
        cases.append((label,args,data,expected,code,error))
    def dump(value):return json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()
    def rows(values):return b''.join(dump(v)+b'\n' for v in values)
    case('JSON array framing',['jobj'],b' [1,{"a":[2,"x,]\\\""]},[3],true,null,"hi"] ',b'1\n{"a":[2,"x,]\\\""]}\n[3]\ntrue\nnull\n"hi"\n')
    case('root object',['jobj'],b'{"a":[1,2]}',b'{"a":[1,2]}\n')
    case('root string',['jobj'],b'"a\\nb"',b'"a\\nb"\n')
    case('empty root array',['jobj'],b'[]',b'')
    case('raw CRLF blank final lines',['jlines'],b'a\r\n\nlast',b'"a"\n""\n"last"\n')
    case('Unicode NUL',['jlines'],'Ж 😀\0\n'.encode(),dump('Ж 😀\0')+b'\n')
    case('raw output Unicode NUL',['jtext'],dump('Ж 😀\0')+b'\n','Ж 😀\0\n'.encode())
    case('mapping arithmetic',['jmap','n => n + 70000'],b'1\n2\n',b'70001\n70002\n')
    case('mutation and arbitrary function',['jmap','o => { o.index++; return o; }'],b'{"index":5}\n',b'{"index":6}\n')
    case('function expression',['jmap','function(s) { return s.toUpperCase(); }'],b'"hello"\n',b'"HELLO"\n')
    case('compiled once state',['jmap','(() => { let n=0; return x => [x,n++]; })()'],b'7\n8\n',b'[7,0]\n[8,1]\n')
    case('diagnostic channel',['jmap','x=>{console.log("note");return x;}'],b'1\n',b'1\n',error=b'note')
    case('map index',['jmap','(x,i)=>i'],b'7\n8\n',b'0\n1\n')
    case('filter truthiness',['jfilter','x => x'],b'0\nfalse\nnull\n""\n[]\n{}\n1\n',b'[]\n{}\n1\n')
    case('reduce seed',['jreduce','--init','0','(a,b)=>a+b'],b'1\n2\n3\n',b'6\n')
    case('empty reduce',['jreduce','--init','{"n":0}','(a,b)=>a'],b'',b'{"n":0}\n')
    case('reduce growing join',['jreduce','--init','""','(a,s,i)=>a+(i?", ":"")+s'],b'"a"\n"b"\n',b'"a, b"\n')
    case('sort numbers',['jsort','(a,b)=>a-b'],b'3\n1\n2\n',b'1\n2\n3\n')
    case('sort stability',['jsort','(a,b)=>a.k-b.k'],b'{"k":2,"n":"a"}\n{"k":1,"n":"b"}\n{"k":2,"n":"c"}\n',b'{"k":1,"n":"b"}\n{"k":2,"n":"a"}\n{"k":2,"n":"c"}\n')
    case('single process stages',['jpipe','--input=json','--filter','o=>o.on','--sort','(a,b)=>a.i-b.i','--map','o=>o.name','--output=text'],b'[{"i":2,"name":"b","on":true},{"i":1,"name":"a","on":true},{"i":0,"name":"skip"}]',b'a\nb\n')
    case('multiple barriers',['jpipe','--sort','(a,b)=>a-b','--reduce','(a,b)=>a+b','--init','0','--map','x=>[x,x*2]'],b'3\n1\n2\n',b'[6,12]\n')
    case('reduce to downstream sort',['jpipe','--reduce','(a,b)=>a+b','--init','0','--sort','(a,b)=>a-b','--map','x=>x+1'],b'3\n2\n',b'6\n')
    case('no fixed stage count',['jpipe',*sum((['--map','x=>x+1'] for _ in range(80)),[])],b'0\n',b'80\n')
    for source in ('x=>undefined','x=>NaN','x=>Infinity','x=>1n','x=>({a:undefined})','x=>[undefined]','x=>function(){}','x=>Symbol("a")','x=>{x.self=x;return x;}'):
        case('non JSON '+source,['jmap',source],b'{}\n',b'',1)
    for source in ('async x=>x','x=>Promise.resolve(x)','x=>{Promise.resolve().then(()=>0);return x;}'):
        case('async rejection '+source,['jmap',source],b'1\n',b'',1,b'async callbacks')
    case('mid-sort cleanup',['jsort','(()=>{let n=0;return(a,b)=>{if(++n===5)throw Error("mid-sort");return a-b;};})()'],b'5\n4\n3\n2\n1\n',b'',1,b'mid-sort')
    case('sort comparator error',['jsort','(a,b)=>{throw Error("compare")}'],b'1\n2\n',b'',1,b'compare')
    case('sort numeric contract',['jsort','(a,b)=>"bad"'],b'1\n2\n',b'',1,b'finite number')
    case('function exception context',['jmap','x=>{if(x===2)throw Error("oops");return x;}'],b'1\n2\n',b'1\n',1,b'stage 1 (map), record 2')
    case('input error context',['jmap','x=>x'],b'1\nbad\n',b'1\n',1,b'input/output record 2')
    case('literal NUL JSON',['jpipe'],b'1\0junk\n',b'',1)
    case('blank JSONL error',['jpipe'],b'\n',b'',1)
    case('not a function',['jmap','42'],b'1\n',b'',1,b'function')
    case('invalid function',['jmap','x => {'],b'1\n',b'',1)
    case('missing reducer seed',['jreduce','(a,b)=>a+b'],b'',b'',2,b'--init')
    case('bad seed',['jreduce','--init','bad','(a,b)=>a+b'],b'',b'',1)
    case('text type contract',['jtext'],b'123\n',b'',1,b'requires strings')
    case('input limit',['jlines','--max-record','8'],b'123456789\n',b'',1,b'--max-record')
    case('output limit',['jmap','--max-record','8','x=>"x".repeat(9)'],b'0\n',b'',1,b'--max-record')
    case('callback budget',['jmap','--max-polls','10','x=>{while(true){}}'],b'0\n',b'',1,b'budget')
    case('compile budget',['jmap','--max-polls','10','(()=>{while(true){} })()'],b'',b'',1,b'budget')
    case('sort memory budget',['jsort','--memory','1048576','(a,b)=>0'],rows(['x'*512]*3000),b'',1,b'memory')
    case('context memory budget',['jpipe','--memory','1024'],b'',b'',1)
    for data in (b'',b'[',b'[1,]',b'[1 2]',b'[1',b'[{"a":]',b'["unterminated]',b'[01]',b'[true] false',b'{} {}',b'["\\uZZZZ"]'):
        # Some records may have been emitted before a later structural error.
        expected=b'1\n' if data in (b'[1,]',b'[1 2]',b'[1') else b'true\n' if data==b'[true] false' else b'{}\n' if data==b'{} {}' else b''
        case('malformed document '+repr(data),['jobj'],data,expected,1)
    rng=random.Random(1337)
    values=[{'i':rng.randrange(12),'s':rng.choice(['Ж 😀','x,]"\\','line\nnext','', '[]{}']),'v':[None,True,rng.randrange(-100000,100000)]} for _ in range(100)]
    case('random nested JSON framing',['jobj'],json.dumps(values,ensure_ascii=False,indent=2).encode(),rows(values))
    case('random stable sort',['jsort','(a,b)=>a.i-b.i'],rows(values),rows(sorted(values,key=lambda x:x['i'])))
    large='Ж' * 36000
    case('cross bank record',['jmap','s=>s.length'],dump(large)+b'\n',b'36000\n')
    def script(i,args):
        return shlex.join(args)+f' < /tmp/in{i} > /tmp/out 2> /tmp/err; echo JPIPE_STATUS:$?; cat /tmp/out; echo JPIPE_STDERR; cat /tmp/err'
    if a.target!='host':
        sys.path.insert(0,str(ROOT/'ports/tls/tests'));from guest import Guest
        files=[dict(name='/bin/'+name,symlink='jpipe') for name in aliases]
        for i,(_,_,data,_,_,_) in enumerate(cases):
            fixture=work/f'in{i}';fixture.write_bytes(data)
            files.append(dict(name=f'/tmp/in{i}',path=str(fixture)))
        files += [dict(name=f'/tmp/case{i}',text=script(i,args)+'\n') for i,(_,args,_,_,_,_) in enumerate(cases)]
        # Guest's JSON manifest accepts text, not host byte buffers.
        for item in files:
            if 'data' in item:item['text']=item.pop('data').decode('utf-8')
        ext='fxe' if a.target=='native' else 'rxe'
        guest=Guest(work,a.target,{'jpipe':out/('jpipe.'+ext)},files,blocks=65536)
    try:
        for i,(label,args,data,expected,code,error) in enumerate(cases):
            if guest:
                result=guest.shell(f'sh /tmp/case{i}',seconds=180).split('\n',1)[1]
                match=re.search(r'^JPIPE_STATUS:(\d+)\n',result,re.M);assert match,(label,result)
                actual_code=int(match[1]);body=result[match.end():];stdout,stderr=body.split('JPIPE_STDERR\n',1)
                stderr=stderr.rsplit(guest.prompt.decode(),1)[0];stdout=stdout.encode();stderr=stderr.encode()
            else:
                result=subprocess.run([str(out/args[0]),*args[1:]],input=data,capture_output=True,timeout=20)
                actual_code,stdout,stderr=result.returncode,result.stdout,result.stderr
            assert actual_code==code and stdout==expected and (error is None or error in stderr),(label,actual_code,stdout[:1000],stderr[:2000],expected[:1000])
            if label=='sort memory budget':assert b'cannot create context' not in stderr,stderr
            results.append(label);print(a.target+': '+label,flush=True)
    finally:
        if guest:
            (a.build.resolve()/(a.target+'-guest.log')).write_bytes(guest.log);guest.close()
print(f'JPIPE_OK {a.target}: {len(results)} semantic/resource cases')
