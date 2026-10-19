#!/usr/bin/env python3
"""Compare guest diff/diff3 with host reference reports and apply their ed scripts."""
import argparse
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
APPS = 'sh echo cat cp ed cmp ls diff diffh diff3'.split()
IMAGE_JS = r'''
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [mode,path,manifest,native]=process.argv.slice(1);
if(mode==='create-rvm') {
 const files=JSON.parse(fs.readFileSync(manifest,'utf8'));
 for(const file of files)if(file.host){file.data=fs.readFileSync(file.host);delete file.host;}
 const root=rootImage(files,32768,1024);
 fs.writeFileSync(path,fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(native+'/FUZIX.SYS')},
  {name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
 process.exit(0);
}
const disk=openImage(path), fat=native==='1'?null:fatFile(disk,'ROOTFS.IMG'), root=fat?fat.data:disk;
if(mode==='seed') {
 const next=installRoot(root,JSON.parse(fs.readFileSync(manifest,'utf8')));
 atomicWrite(path,fat?fat.replace(next):next);
} else {
 const checked=checkRoot(root);if(checked.issues.length)throw Error(checked.issues.join('\n'));
 function cells(node){const a=Array(node.size).fill(0);for(const [index,block] of node.map)
  for(let i=0;i<1024&&index*1024+i<a.length;i++)a[index*1024+i]=root.readUInt16LE((block*1024+i)*2);return a;}
 function entries(node){const a=cells(node),out={};for(let i=0;i<a.length;i+=32)if(a[i]){
  let n='';for(let j=1;j<31&&a[i+j];j++)n+=String.fromCharCode(a[i+j]);out[n]=a[i];}return out;}
 function lookup(path){let n=checked.nodes.get(1);for(const p of path.split('/').filter(Boolean))n=checked.nodes.get(entries(n)[p]);return n;}
 const out={};for(const path of JSON.parse(fs.readFileSync(manifest,'utf8')))out[path]=cells(lookup(path));
 out['/tmp-entries']=Object.keys(entries(lookup('/tmp')));console.log(JSON.stringify(out));
}
'''


def run_node(*args):
    result=subprocess.run(['node', '--input-type=module', '-e', IMAGE_JS, *map(str,args)],
                          cwd=ROOT, capture_output=True, text=True)
    assert result.returncode==0,result.stderr
    return result.stdout


def normalized_report(text):
    """V7 and BSD print file 2/3 sections in different orders for ====2."""
    chunks=re.split(r'(?m)(^====[^\n]*\n)',text)
    result=[]
    for at in range(1,len(chunks),2):
        sections=re.split(r'(?m)(?=^[123]:)',chunks[at+1])
        result.append(chunks[at]+''.join(sorted(filter(None,sections))))
    return ''.join(result)


def merged_reference(triple, report, mode):
    """Apply selected ranges from the reference report, independently of ed.

    The host's old diff3 -x has additional bugs, so use its comparison ranges
    and the original input text rather than trusting its generated scripts.
    """
    output=triple[0].splitlines(keepends=True)
    third=triple[2].splitlines(keepends=True)
    chunks=re.split(r'(?m)^====([^\n]*)\n',report)
    edits=[]
    for at in range(1,len(chunks),2):
        tag=chunks[at]
        if tag not in ({'','3'} if mode=='-e' else {''} if mode=='-x' else {'3'}):continue
        ranges=[]
        for number in (1,3):
            match=re.search(rf'(?m)^{number}:(\d+)(?:,(\d+))?([ac])$',chunks[at+1])
            first,last=int(match[1]),int(match[2] or match[1])
            ranges.append((first,first) if match[3]=='a' else (first-1,last))
        edits.append((*ranges[0],third[ranges[1][0]:ranges[1][1]]))
    for first,last,text in reversed(edits):output[first:last]=text
    return ''.join(output)


def suite(work, focused=False):
    fixtures, expected, commands, scripts = [], {}, [], {}
    def file(path, text):
        data=list(map(ord,text));fixtures.append(dict(name=path,mode=0o100644,data=data));expected[path]=data
    def check(command, out, text, status):
        commands.append(f'{command} > {out}; echo $? > {out}-status')
        expected[out]=list(map(ord,text));expected[out+'-status']=list(map(ord,str(status)+'\n'))
    cases=[('same\n',)*3, ('',)*3, ('a\n','a\n','b\n'),('a\n','b\n','c\n'),
           ('a\n','b\n','b\n'),('b\n','a\n','b\n'),('', '', 'insert\n'),
           ('a\n','a\n',''),('a\nb\nc\n','a\nx\nb\nc\n','a\nb\ny\nc\n'),
           ('a\nb\nc\n','a\nc\n','a\nx\nc\n'),('end','end2','end3'),
           ('end','end','end\n'),('one\n.\nend\n','one\nold\nend\n','one\n.\n.\nnew\nend\n'),
           ('\u1234\u20ac\n','different\n','\u1234\u20ac\n')]
    rng=random.Random(371)
    for _ in range(8):
        variants=[]
        for k in range(3):
            lines=[f'line {i}\n' for i in range(14)]
            for j in range(3):
                at=rng.randrange(len(lines));lines[at:at+rng.randrange(3)]=[f'edit {k}.{j}.{rng.randrange(9)}\n']*rng.randrange(3)
            variants.append(''.join(lines))
        cases.append(tuple(variants))
    # More than 200 independent hunks and offsets beyond 64K.
    cases.append(tuple(''.join(f'{i}: {k if i%2 else "same"}\n' for i in range(510)) for k in 'abc'))
    cases.append(('x'*35000+'\nlast\n','y'*35000+'\nlast\n','z'*35000+'\nlast\n'))
    cases.append(tuple(''.join(f'{i:04d} '+('line' if i!=350 else k)*20+'\n' for i in range(700)) for k in 'abc'))
    for n, triple in enumerate(cases):
        folder=f'/c{n}';host=[]
        for k,text in enumerate(triple):
            path=f'{folder}/{k}';file(path,text)
            h=work/f'host-{n}-{k}';h.write_text(text);host.append(str(h))
        paths=[f'{folder}/{k}' for k in range(3)]
        # Exec/load cost is substantial on RVM. Keep the full semantic matrix
        # on Fuse-X and run the width/limits/format edge cases on both backends.
        if focused and n not in (0,1,3,6,7,10,12,22,23,24):continue
        # A pathname containing quotes and shell punctuation must reach execv unchanged.
        if n==3:
            paths[0]="/c3/a ' ;";file(paths[0],triple[0])
        quoted=' '.join(map(shlex.quote,paths))
        diff=subprocess.run(['/usr/bin/diff',host[0],host[2]],capture_output=True,text=True)
        check('diff -- '+shlex.quote(paths[0])+' '+paths[2],f'{folder}/diff',diff.stdout,diff.returncode)
        ref=subprocess.run(['/usr/bin/diff3',*host],capture_output=True,text=True)
        if n==10:
            ref.stdout='====\n'+''.join(f'{i+1}:1c\n  {s}\n\\ No newline at end of file\n' for i,s in enumerate(triple));ref.returncode=0
        if n==11:
            ref.stdout='====3\n1:1c\n2:1c\n  end\n\\ No newline at end of file\n3:1c\n  end\n';ref.returncode=0
        assert ref.returncode==0,(n,ref.stderr)
        for mode in ['', '-e','-x','-3']:
            if focused and mode and not (n==3 or mode=='-e' and n in (12,22)):continue
            if mode and any(s and not s.endswith('\n') for s in triple):continue
            out=f'{folder}/d3'+(mode or 'report')
            if not mode:check('diff3 '+quoted,out,ref.stdout,0)
            else:
                commands.append(f'diff3 {mode} {quoted} > {out}; echo $? > {out}-status')
                expected[out+'-status']=list(map(ord,'0\n'))
                result=merged_reference(triple,ref.stdout,mode)
                dest=out+'-applied'
                scripts[out]=(triple[0],result)
                if n in ((3,12) if focused else (0,3,12,14)):
                    commands.append(f"(cat {out}; echo 'w {dest}'; echo Q) | ed {shlex.quote(paths[0])}")
                    expected[dest]=list(map(ord,result))
        if (not focused or n in (3,12)) and all(not s or s.endswith('\n') for s in triple):
            commands.append(f'diff -e -- {shlex.quote(paths[0])} {paths[2]} > {folder}/ed; echo $? > {folder}/ed-status')
            expected[f'{folder}/ed-status']=list(map(ord,str(diff.returncode)+'\n'))
            scripts[f'{folder}/ed']=(triple[0],triple[2])
            if n in (0,3,12,14):
                commands.append(f"(cat {folder}/ed; echo 'w {folder}/ed-applied'; echo Q) | ed {shlex.quote(paths[0])}")
                expected[f'{folder}/ed-applied']=list(map(ord,triple[2]))
    # Force the full sorter above the signed-16-bit doubling boundary without
    # generating ambiguous matches or exhausting the candidate budget.
    file('/sort-left','left\n'*17000);file('/sort-right','right\n'*17000)
    check('diff /sort-left /sort-right','/sort-diff',
          '1,17000c1,17000\n'+'< left\n'*17000+'---\n' + '> right\n'*17000,1)
    check('diff -h /c3/0 /c3/2','/heuristic','1,$c1,$\n< a\n---\n> c\n',1)
    file('/blanks','a  b\n');file('/blanks2','a b\n')
    check('diff -b -- /blanks /blanks2','/whitespace','',0)
    check('cat /c3/0 | diff - /c3/0','/diff-stdin','',0)
    check('cat /c3/0 | diff3 - /c3/1 /c3/2','/stdin', '====\n1:1c\n  a\n2:1c\n  b\n3:1c\n  c\n',0)
    for i,command in enumerate(['diff3','diff3 -m /c0/0 /c0/1 /c0/2','diff3 /missing /c0/1 /c0/2',
                                'diff3 / /c0/1 /c0/2','cat /c0/0 | diff3 - - /c0/0','diff -- /missing /c0/0',
                                'diff3 -e /c10/0 /c10/1 /c10/2','diff -e /c10/0 /c10/2']):
        check(command,f'/bad{i}','',2)
    commands+=['ls /tmp > /tmp-list','echo DIFF_SUITE_DONE','shutdown']
    fixtures.append(dict(name='/bin/diff-suite',mode=0o100755,data=list(map(ord,'\n'.join(commands)+'\n'))))
    return fixtures,expected,scripts


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['fuzix','fuse-x','both'],default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    args=p.parse_args()
    for system in (['fuse-x','fuzix'] if args.os=='both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='diff-suite-') as t:
            work=Path(t);image=work/'disk.img';stage=work/'stage';stage.mkdir()
            native=system=='fuse-x';fixtures,expected,scripts=suite(work,focused=not native)
            build=(args.native_build if native else args.build).resolve()
            if native:
                apps=stage/'apps';apps.mkdir()
                for name in [*APPS,'native-hello']:(apps/(name+'.fxe')).symlink_to(build/'apps'/(name+'.fxe'))
                (stage/'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in APPS]))
                subprocess.run(['node',str(ROOT/'fuse-x/image.mjs'),'create',str(image),'--apps',str(apps)],check=True,capture_output=True)
                command=[str(build/'fuse-x'),'--run',str(image)]
            else:
                fixtures += [dict(name='/init' if n=='init' else '/bin/'+n,mode=0o100755,host=str(build/(n+'.rxe'))) for n in ['init',*APPS]]
                fixtures += [dict(name=n,mode=0o20666,device=d) for n,d in [('/dev/tty',0x200),('/dev/console',0x201),('/dev/null',0x100)]]
                command=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000','--headless','--net','off','--jit','--timeout','360','--input','sh /bin/diff-suite\n']
            manifest=work/'files.json';manifest.write_text(json.dumps(fixtures))
            if native:run_node('seed',image,manifest,1)
            else:run_node('create-rvm',image,manifest,build)
            logpath=ROOT/'build/fuzix'/('diff-'+system+'.log')
            with logpath.open('w') as stream:
                result=subprocess.run(command,input='sh /bin/diff-suite\n' if native else None,stdout=stream,stderr=subprocess.STDOUT,text=True,timeout=370,
                                      env=dict(os.environ,RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET='off',RCC_NATIVE_TIMEOUT='360'))
            log=logpath.read_text()
            assert result.returncode==0 and 'DIFF_SUITE_DONE' in log and 'panic:' not in log,log[-10000:]
            manifest.write_text(json.dumps(list(expected)+list(scripts)));actual=json.loads(run_node('read',image,manifest,int(native)))
            for path,wanted in expected.items():
                if path.endswith('d3report'):
                    assert normalized_report(''.join(map(chr,actual[path])))==normalized_report(''.join(map(chr,wanted))),(system,path,actual[path][:200],wanted[:200])
                    continue
                assert actual[path]==wanted,(system,path,''.join(map(chr,actual[path]))[:1200],''.join(map(chr,wanted))[:1200],str(logpath))
            for path,(original,wanted) in scripts.items():
                applied=work/'apply-guest-script';applied.write_text(original)
                script=''.join(map(chr,actual[path]))
                ed=subprocess.run(['/bin/ed',str(applied)],input=script+'w\nQ\n',capture_output=True,text=True)
                assert ed.returncode==0 and applied.read_text()==wanted,(system,path,script[:1200],ed.stdout)
            assert not any(n.startswith(('d3','diff')) for n in actual['/tmp-entries']),actual['/tmp-entries']
            if native:
                sys.path.insert(0,str(ROOT/'fuse-x'))
                from test_support import record_runtime
                record_runtime(build,['diff','diffh','diff3'],'regression tested')
            print(f'{system}: {len(expected)} file/status assertions; reference comparisons, 255 hunks, >64K offsets, 35K-character lines, 17K-line sorting, ed scripts/dots, stdin, quoted paths, errors, cleanup and clean filesystem passed')

if __name__=='__main__':main()
