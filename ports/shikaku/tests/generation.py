#!/usr/bin/env python3
"""Independently prove uniqueness of runtime C-generator fixtures."""
import importlib.util,subprocess,tempfile,os,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('shikaku_generator',ROOT/'ports/shikaku/generate.py')
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
with tempfile.TemporaryDirectory(prefix='shikaku-proof-') as directory:
    binary=Path(directory)/'check'
    sdk=os.environ.get('SDKROOT') or (subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip() if sys.platform=='darwin' else '')
    sdk_flags=['-isysroot',sdk] if sdk else []
    subprocess.run(['clang',*sdk_flags,'-O2','-std=c99','-Wall','-Wextra','-Werror','-Iports/shikaku','ports/shikaku/tests/engine.c','ports/shikaku/engine.c','ports/shikaku/store.c','ports/shikaku/generator.c','-o',str(binary)],cwd=ROOT,check=True)
    result=subprocess.check_output([str(binary)],env=dict(os.environ,SHIKAKU_DUMP='1'),text=True)
    count=0
    for line in result.splitlines():
        if not line.startswith('SHIKAKU_CLUES '):continue
        n,seed,*cells=map(int,line.split()[1:]);assert len(cells)==n*n
        clues={i:v for i,v in enumerate(cells) if v}
        assert len(generator.solutions(n,clues))==1,(n,seed)
        count+=1
    assert count==18
    print(result.splitlines()[-1])
    print(f'{count} runtime-generated boards independently proved uniquely solvable')
