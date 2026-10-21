#!/usr/bin/env python3
"""Variadic module ABI, malformed metadata and SDK startup rejection checks.

Execution of mixed carriers/callbacks/recursion/70 tails/bank edges is covered
by native-sdk/test.py's full in-guest source build, in addition to test.py.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from test_modules import module_location
HERE=Path(__file__).resolve().parent
BASE=HERE.parent/'build'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tools',type=Path,default=BASE)
    base=p.parse_args().tools.resolve()
    with tempfile.TemporaryDirectory(prefix='native-varargs-') as folder:
        w=Path(folder);tools=w/'tools';tools.mkdir()
        for n in ['rcc-c','rcc-qbe','rcc-x64-as','rcc-x64-ld']:(tools/n).symlink_to(base/n)
        (tools/'qbe').symlink_to(BASE/'qbe-host/qbe')
        cc=base/'rcc-native-cc';ld=base/'rcc-x64-ld'
        def compile(name,text,sdk=False):
            src=w/(name+'.c');src.write_text(text)
            cmd=[str(cc),'--tools',str(tools),'-c',str(src),'-o',str(w/(name+'.o'))]
            if sdk:cmd+=['--sdk',str(w/'sdk')]
            subprocess.run(cmd,check=True)
            return w/(name+'.o')
        consumer=compile('main','extern int f(int,...); int main(void) { return f(7,9); }')
        fixed=compile('fixed','int f(int x,void *p) { return x; }')
        variadic=compile('variadic','int f(int x,...) { return x; }')
        subprocess.run([str(ld),str(consumer),str(variadic),'-o',str(w/'valid')],check=True)
        rejects=0
        def reject(args):
            nonlocal rejects
            out=w/'keep';out.write_bytes(b'preserved')
            r=subprocess.run([str(ld),*map(str,args),'-o',str(out)],capture_output=True)
            assert r.returncode==1 and r.stderr and out.read_bytes()==b'preserved',r
            assert not list(w.glob('keep.tmp.*'))
            rejects+=1
        reject([consumer,fixed])
        raw=variadic.read_bytes();at,size,rows,*_=module_location(raw)
        row=next(r for r in rows if r['kind']==1)
        assert row['signature']=='i16(i16,...)' and row['cells']==2
        start=at+row['offset']+64+len(row['name'])+1
        for bad in [b'i16(...,i16)',b'i16(i16,..x)',b'i16(i16,..,)']:
            assert len(bad)==len(row['signature'])
            data=bytearray(raw);data[start:start+len(bad)]=bad
            path=w/'bad.o';path.write_bytes(data);reject([consumer,path])
        # A program already carrying the minimal entry may not be silently
        # combined with a libc startup. Missing/ambiguous main is also fatal.
        sdk=w/'sdk';sdk.mkdir();(sdk/'include').mkdir()
        no_main=compile('library','int helper(void) { return 0; }',True)
        reject(['--sdk',sdk,consumer,variadic])
        reject(['--sdk',sdk,no_main])
    print(f'Variadic scalar ABI and SDK startup: {rejects} preserved-output rejections passed')


if __name__=='__main__':main()
