#!/usr/bin/env python3
"""Portable Unicode checks plus the complete pinned Unicode 16 grapheme suite."""
import argparse,ctypes as C,os,shlex,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default=os.environ.get('CC','cc'))
a=p.parse_args();flags=['-std=c99','-Wall','-Wextra','-Werror']
if sys.platform=='darwin':
    flags+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()]
with tempfile.TemporaryDirectory(prefix='unicode-test-') as t:
    exe=Path(t)/'check';lib=Path(t)/'unicode.so'
    engine=ROOT/'libraries/unicode/unicode.c'
    subprocess.run([*shlex.split(a.cc),*flags,'-fsanitize=undefined',ROOT/'libraries/tests/unicode.c',engine,ROOT/'libraries/unicode/font.c','-o',exe],check=True)
    subprocess.run([exe],check=True)
    subprocess.run([*shlex.split(a.cc),*flags,'-fPIC','-shared',engine,'-o',lib],check=True)
    unicode=C.CDLL(str(lib))
    unicode.ru_next_grapheme.argtypes=[C.c_char_p,C.c_size_t,C.c_size_t,C.POINTER(C.c_uint)]
    unicode.ru_next_grapheme.restype=C.c_size_t
    count=0
    for line in (ROOT/'libraries/unicode/GraphemeBreakTest.txt').read_text().splitlines():
        fields=line.split('#')[0].split()
        if not fields:continue
        data=bytearray();expected=[]
        for field in fields:
            if field=='÷':expected.append(len(data))
            elif field!='×':data.extend(chr(int(field,16)).encode())
        at=0;actual=[0]
        while at<len(data):
            at=unicode.ru_next_grapheme(bytes(data),len(data),at,None);actual.append(at)
        assert actual==expected,(line,expected,actual)
        count+=1
    print(f'unicode: {count} official Unicode 16 grapheme vectors passed')
