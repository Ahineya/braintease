#!/usr/bin/env python3
"""Compile native tool sources through the new pipeline against its C16 SDK.

This host assessment is not a guest self-build. It exposes remaining language,
code-size and memory gates before paying for the emulated source rebuild.
"""
import argparse
import json
from pathlib import Path
import subprocess
from source_tools import tool_sources
HERE=Path(__file__).resolve().parent
PORTABLE=HERE.parent
ROOT=PORTABLE.parent


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=ROOT/'build/portable-native-sdk')
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-self-assessment/current')
    p.add_argument('--link',action='store_true',help='also host-bootstrap four executable tools with this backend and SDK')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if a.link:(out/'link-result.json').unlink(missing_ok=True)
    sdk=a.sdk.resolve();base=PORTABLE/'build'
    sources=tool_sources()
    results=[]
    for i,src in enumerate(sources):
        source=src
        if src==PORTABLE/'ripple/parse.c':
            # Match native/build.py's existing private bootstrap declaration.
            # The corresponding conversion implementation is linked explicitly
            # when rebuilding the tool; this assessment stops at objects.
            source=out/'parser.c'
            source.write_text('unsigned long long strtoull(const char *,char **,int);\n#include "'+str(src)+'"\n')
        command=[str(base/'rcc-native-cc'),'--tools',str(sdk/'tools'),'--sdk',str(sdk),'-c',str(source),'-o',str(out/f'o{i}.o')]
        r=subprocess.run(command,capture_output=True,text=True)
        (out/f'o{i}.log').write_text(r.stderr)
        results.append(dict(source=str(src.relative_to(ROOT)),status='passed' if not r.returncode else 'blocked',diagnostic=r.stderr.splitlines()[:3]))
        print(results[-1],flush=True)
    failed=any(r['status']!='passed' for r in results)
    (out/'result.json').write_text(json.dumps(dict(status='blocked' if failed else 'passed',units=results),indent=2)+'\n')
    if failed:raise SystemExit(1)
    if a.link:
        support=out/'strtoull.o'
        subprocess.run([str(base/'rcc-native-cc'),'--tools',str(sdk/'tools'),'--sdk',str(sdk),
                        '-c',str(PORTABLE/'guest/strtoull.c'),'-o',str(support)],check=True)
        groups={'rcc-qbe':[out/f'o{i}.o' for i,s in enumerate(sources) if s.parent.name in ['native','ripple']]+[support],
                'rcc-x64-ld':[out/f'o{i}.o' for i,s in enumerate(sources) if s.parent.name=='x64-link'],
                'rcc-native-cc':[out/f'o{i}.o' for i,s in enumerate(sources) if s.parent.name=='native-driver'],
                'rcc-x64-ar':[out/f'o{i}.o' for i,s in enumerate(sources) if s.parent.name=='native-archive']}
        links=[]
        for name,objects in groups.items():
            output=out/(name+'.fxe')
            r=subprocess.run([str(base/'rcc-x64-ld'),'--sdk',str(sdk),*map(str,objects),'-o',str(output)],capture_output=True,text=True)
            links.append(dict(tool=name,status='passed' if not r.returncode else 'blocked',diagnostic=r.stderr,
                              size=output.stat().st_size if not r.returncode else None))
        (out/'link-result.json').write_text(json.dumps(links,indent=2)+'\n')
        if any(r['status']!='passed' for r in links):raise SystemExit(1)


if __name__=='__main__':main()
