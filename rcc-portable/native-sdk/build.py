#!/usr/bin/env python3
"""Prepare raw C16 libc sources/headers and compile them with portable native tools.

Host mode bootstraps the SDK; guest acceptance must rebuild the listed sources
inside x86, not reuse these objects as proof of a guest-built library.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from make_rules import header_rules

HERE=Path(__file__).resolve().parent
PORTABLE=HERE.parent
ROOT=PORTABLE.parent
sys.path.insert(0,str(PORTABLE))
from guest_build import support


def prepare(out):
    tree=out/'tree'
    licenses=out/'licenses';licenses.mkdir(exist_ok=True)
    for source,name in [('ports/fuzix/upstream/LICENCE','FUZIX-GPL.txt'),
                        ('ports/fuzix/upstream/Library/COPYING','FUZIX-LGPL.txt')]:
        shutil.copyfile(ROOT/source,licenses/name)
    bases=['ports/fuzix/ripple/include','ports/fuzix/upstream/Library/include','runtime/include']
    for relative in bases+['ports/fuzix/upstream/Kernel/include','fuse-x/include']:
        shutil.copytree(ROOT/relative,tree/relative,dirs_exist_ok=True)
    for relative in [str(p.relative_to(ROOT)) for p in (ROOT/'ports/fuzix/ripple').glob('*.h')]:
        p=tree/relative;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/relative,p)
    inc=out/'include';inc.mkdir(exist_ok=True)
    copied=set()
    for base in bases:
        for header in (ROOT/base).rglob('*.h'):
            name=header.relative_to(ROOT/base)
            if name in copied:continue
            copied.add(name);dest=inc/name;dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_text('#include "'+os.path.relpath(tree/header.relative_to(ROOT),dest.parent)+'"\n')
    for header in (ROOT/'ports/fuzix/ripple').glob('*.h'):
        if Path(header.name) not in copied:
            (inc/header.name).write_text('#include "'+os.path.relpath(tree/header.relative_to(ROOT),inc)+'"\n')
    excluded=set('regerror malloc calloc realloc readline readline_fn clock_gettime clock_getres __sin __cos __rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow sin cos atan atan2 strtod ecvt fmod frexp ldexp modf'.split())
    sources=[ROOT/('ports/fuzix/ripple/libc/'+n+'.c') for n in 'syscalls stdio scan wait clock timezone dirent misc heap pipe sockets resolver search identity'.split()]
    sources += sorted(p for p in (ROOT/'ports/fuzix/upstream/Library/libs').glob('*.c') if p.stem not in excluded)
    sources += [ROOT/'ports/fuzix/ripple/userlib.c',ROOT/'fuse-x/libc/bridge.c',ROOT/'libraries/unicode/unicode.c']
    rows=[]
    for i,src in enumerate(sources):
        rel=src.relative_to(ROOT);dest=tree/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
        # Upstream strrchr's C interface returns char*, but its local traversal
        # pointer is const char*. Make the required explicit cast in the copy.
        if src.name=='strrchr.c':
            text=dest.read_text();assert text.count('return p;')==1
            dest.write_text(text.replace('return p;','return (char *)p;'))
        if src.name=='syscalls.c':
            text=dest.read_text();old='int utime(const char *p, struct utimbuf *t)'
            assert text.count(old)==1
            dest.write_text(text.replace(old,'int utime(const char *p, const struct utimbuf *t)'))
        # Banked scalar C fallback remains available without extra host imports.
        flags=['-UFUSE_X_USER','-DRIPPLE_MMIO_NO_INLINE=1'] if src.name=='userlib.c' else []
        rows.append(dict(source=str(dest.relative_to(out)),object=f'objects/o{i}.o',flags=flags))
    for src in (ROOT/'libraries/unicode').glob('*.inc'):shutil.copyfile(src,tree/'libraries/unicode'/src.name)
    for src in (ROOT/'ports/fuzix/upstream/Library/libs').glob('*.h'):
        shutil.copyfile(src,tree/'ports/fuzix/upstream/Library/libs'/src.name)
    src=ROOT/'ports/fuzix/upstream/Applications/netd/netdb.h'
    dest=tree/src.relative_to(ROOT);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
    fmt=support(out/'generated')
    # Keep even the currently disabled floating-format include relocatable;
    # the source SDK must not embed a path to the bootstrap machine's tree.
    float_include=ROOT/'ports/fuzix/ripple/float-format.inc'
    text=fmt.read_text();old='#include "'+str(float_include)+'"'
    assert text.count(old)==1
    fmt.write_text(text.replace(old,'#include "float-format.inc"'))
    shutil.copyfile(float_include,out/'generated/float-format.inc')
    rows.append(dict(source=str(fmt.relative_to(out)),object=f'objects/o{len(rows)}.o',flags=[]))
    cells=HERE/'cells.c';shutil.copyfile(cells,out/'generated/cells.c')
    rows.append(dict(source='generated/cells.c',object=f'objects/o{len(rows)}.o',flags=[]))
    for arity in [0,2,3]:
        params=['int','char **','char **'][:arity];values=['a->argc','a->argv','a->envp'][:arity]
        src=out/f'generated/crt0-{arity}.c'
        src.write_text('#include <stdlib.h>\nstruct arguments {unsigned argc;char **argv,**envp;};\n'
                       'extern void ripple_libc_init(int,char**,char**);\n'
                       'extern int main('+(','.join(params) or 'void')+');\n'
                       'void __rcc_libc_entry(unsigned long pointer) {\n'
                       'struct arguments *a=(void*)pointer;\nripple_libc_init(a->argc,a->argv,a->envp);\n'
                       'exit(main('+','.join(values)+'));\nfor(;;){}\n}\n')
        rows.append(dict(source=str(src.relative_to(out)),object=f'crt0-{arity}.o',flags=[]))
    (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
    headers=sorted(str(p.relative_to(out)) for folder in ['include','tree','generated']
                   for p in (out/folder).rglob('*') if p.suffix in ['.h','.inc'])
    objects=[r['object'] for r in rows if r['object'].startswith('objects/')]
    ready,dependencies,stamps=header_rules(headers,'.headers')
    make=['# Generated source SDK; run make in this directory inside Riplix.',
          'CC = rcc-native-cc','AR = rcc-x64-ar','SDK = .',
          '.PHONY: all clean',
          'all: libc.a crt0-0.o crt0-2.o crt0-3.o',
          'libc.a: '+' '.join(objects),'\t$(AR) -o libc.a '+' '.join(objects)]
    make+=dependencies
    for row in rows:
        make += [row['object']+': '+row['source']+' '+ready+' Makefile',
                 '\tmkdir -p objects',
                 '\t$(CC) --sdk $(SDK) '+' '.join(row['flags'])+' -c '+row['source']+' -o '+row['object']]
    make += ['clean:','\trm -f libc.a '+' '.join(r['object'] for r in rows)+' '+' '.join(stamps)]
    (out/'Makefile').write_text('\n'.join(make)+'\n')
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-sdk')
    p.add_argument('--tools',type=Path,default=PORTABLE/'build')
    p.add_argument('--prepare-only',action='store_true')
    p.add_argument('--assess',action='store_true',help='emit all diagnostics without assembling or publishing libc')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    rows=prepare(out)
    if a.prepare_only:return
    tools=out/'tools';tools.mkdir(exist_ok=True);base=a.tools.resolve()
    for n in ['rcc-c','rcc-qbe','rcc-x64-as','rcc-x64-ld']:
        f=tools/n;f.unlink(missing_ok=True);f.symlink_to(base/n)
    f=tools/'qbe';f.unlink(missing_ok=True);f.symlink_to((PORTABLE/'build/qbe-host/qbe').resolve())
    results=[];failed=False
    for row in rows:
        src=out/row['source'];obj=out/row['object'];obj.parent.mkdir(exist_ok=True)
        command=[str(base/'rcc-native-cc'),'--tools',str(tools),'--sdk',str(out),*row['flags'],
                 '--emit-ir' if a.assess else '-c',str(src),'-o',str(obj.with_suffix('.ir') if a.assess else obj)]
        r=subprocess.run(command,capture_output=True,text=True)
        if a.assess and r.returncode==0:
            r=subprocess.run([str(base/'rcc-qbe'),'--relocatable','--no-start',str(obj.with_suffix('.ir')),'-o',str(obj.with_suffix('.ssa'))],capture_output=True,text=True)
        obj.with_suffix('.log').write_text(r.stderr)
        if r.returncode:failed=True;print(row['source']+': '+r.stderr.splitlines()[0],flush=True)
        else:print(row['source']+': passed',flush=True)
        results.append(dict(**row,status='passed' if not r.returncode else 'blocked',diagnostic=r.stderr.splitlines()[:4]))
    (out/'result.json').write_text(json.dumps(dict(status='blocked' if failed else 'passed',units=results),indent=2)+'\n')
    if failed:raise SystemExit(1)
    if not a.assess:
        objects=[out/r['object'] for r in rows if r['object'].startswith('objects/')]
        subprocess.run([str(base/'rcc-x64-ar'),'-o',str(out/'libc.a'),*map(str,objects)],check=True)
    print(out)


if __name__=='__main__':main()
