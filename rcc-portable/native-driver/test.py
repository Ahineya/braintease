#!/usr/bin/env python3
"""Driver stage ordering, option forwarding, failure propagation and cleanup."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,default=HERE.parent/'build/rcc-native-cc')
    a=p.parse_args();tool=str(a.tool.resolve())
    with tempfile.TemporaryDirectory(prefix='native driver test ') as folder:
        w=Path(folder);tools=w/'tools';tools.mkdir();log=w/'log';output=w/'program';source=w/'input file.c'
        source.write_text('int main(void) { return 0; }\n')
        names=['rcc-c','rcc-qbe','qbe','rcc-x64-as','rcc-x64-ld']
        for name in names:
            f=tools/name
            f.write_text('#!'+sys.executable+'\n'+'''import json,os,sys
from pathlib import Path
name=Path(sys.argv[0]).name
with open(os.environ['DRIVER_LOG'],'a') as f:f.write(json.dumps([name,*sys.argv[1:]])+'\\n')
if name==os.environ.get('FAIL_STAGE'):sys.exit(17)
Path(sys.argv[sys.argv.index('-o')+1]).write_text('stage '+name)
''');f.chmod(0o755)
        env=dict(os.environ,RCC_NATIVE_TOOLS=str(tools),DRIVER_LOG=str(log))
        def run(options=(),fail=None):
            log.write_text('');output.write_text('preserve')
            e=dict(env)
            if fail:e['FAIL_STAGE']=fail
            r=subprocess.run([tool,*options,str(source),'-o',str(output)],env=e,capture_output=True,text=True)
            rows=[json.loads(line) for line in log.read_text().splitlines()]
            temps=[arg for row in rows for arg in row if arg.startswith('/tmp/rcc-native-')]
            assert all(not Path(arg).exists() and not Path(arg).parent.exists() for arg in temps),'temporary files/directory leaked'
            return r,rows
        r,rows=run(['-I','include dir','-DANSWER=37','-U','UNWANTED','-v'])
        assert r.returncode==0 and [row[0] for row in rows]==names
        assert rows[0][1:3]==['--target','native']
        assert rows[0][3:8]==['-I','include dir','-DANSWER=37','-U','UNWANTED']
        assert '--metadata' not in rows[-1] and '--relocatable' in rows[1] and 'amd64_sysv' in rows[2]
        assert output.read_text()=='stage rcc-x64-ld'
        r,rows=run(['--sdk','sdk path','-DUSER_FLAG=1'])
        assert r.returncode==0
        assert rows[0][3:9]==['-DRIPPLE_MMU=1','-DFUSE_X_USER=1','-DFUSE_X_BOUNDED_HEAP=1','-DUSER_FLAG=1','-I','sdk path/include']
        assert '--no-start' in rows[1] and rows[-1][1:3]==['--sdk','sdk path']
        r,rows=run(['--sdk','sdk path','-I','custom headers'])
        assert r.returncode==0
        assert rows[0].index('custom headers') < rows[0].index('sdk path/include'), 'SDK headers override explicit user include path'
        env['RCC_NATIVE_SDK']='environment sdk'
        r,rows=run(['--sdk','explicit sdk','-c'])
        assert r.returncode==0 and 'explicit sdk/include' in rows[0]
        assert '--no-start' in rows[1] and len(rows)==4
        r,rows=run(['--emit-qbe'])
        assert r.returncode==0 and 'environment sdk/include' in rows[0] and '--no-start' in rows[1]
        del env['RCC_NATIVE_SDK']
        for flag,count in [('-E',1),('--emit-ir',1),('--emit-qbe',2),('-S',3),('-c',4)]:
            r,rows=run([flag]);assert r.returncode==0 and len(rows)==count
            assert rows[-1][-1]==str(output) if flag!='-S' else rows[-1][-2]==str(output)
        for name in names:
            r,rows=run(fail=name)
            assert r.returncode==17 and rows[-1][0]==name
            assert output.read_text()=='preserve'
        for flags in [['--target','fuse-x'],['-o'],['-E',str(source)],['-I']]:
            r=subprocess.run([tool,str(source),*flags],env=env,capture_output=True)
            assert r.returncode==1 and r.stderr
        r=subprocess.run([tool,str(source),'-o',str(source)],env=env,capture_output=True)
        assert r.returncode==1 and source.read_text().startswith('int main')
        first=w/'first.o';second=w/'second.o';first.write_text('one');second.write_text('two')
        log.write_text('')
        r=subprocess.run([tool,str(first),str(second),'-o',str(output)],env=env,capture_output=True)
        rows=[json.loads(line) for line in log.read_text().splitlines()]
        assert r.returncode==0 and rows==[['rcc-x64-ld',str(first),str(second),'-o',str(output)]]
        library=w/'libtest.a';library.write_text('archive');log.write_text('')
        r=subprocess.run([tool,str(first),str(library),'-o',str(output)],env=env,capture_output=True)
        rows=[json.loads(line) for line in log.read_text().splitlines()]
        assert r.returncode==0 and rows==[['rcc-x64-ld',str(first),str(library),'-o',str(output)]]
        r=subprocess.run([tool,'-c',str(library),'-o',str(output)],env=env,capture_output=True)
        assert r.returncode==1 and r.stderr
        log.write_text('')
        r=subprocess.run([tool,str(source),str(second),'-o',str(output)],env=env,capture_output=True)
        rows=[json.loads(line) for line in log.read_text().splitlines()]
        assert r.returncode==0 and [row[0] for row in rows]==names
        assert rows[-1][-3]==str(second)
    print('Native driver ordering, all stop modes, options, failures and private temporary cleanup passed')


if __name__=='__main__':main()
