#!/usr/bin/env python3
"""Compare the independent parser with Python's CSV library on generated records."""
from pathlib import Path
import csv,io,json,random,subprocess,tempfile,sys,hashlib
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent
out=ROOT/'build/lvgl/csv-model-host'
cmd=['/usr/bin/clang','-std=c99','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all'] if sys.platform=='darwin' else ['cc','-std=c99','-O1','-fsanitize=undefined','-fno-sanitize-recover=all']
if sys.platform=='darwin':cmd+=['-isysroot','/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
subprocess.run([*cmd,str(PORT/'csv.c'),str(PORT/'csv-test.c'),'-o',str(out)],check=True)
subprocess.run([str(out)],check=True)
rng=random.Random(381);atoms=['','alpha','Київ','π','a,b','two\nlines','a\rb','quote " inside','tab\tinside','semi;colon','0','-2.5','🙂']
with tempfile.TemporaryDirectory(prefix='csv-model-') as directory:
    for case in range(150):
        delimiter=[',','\t',';'][case%3];rows=[[rng.choice(atoms) for _ in range(rng.randint(1,12))] for _ in range(rng.randint(1,25))]
        text=io.StringIO(newline='');csv.writer(text,delimiter=delimiter,lineterminator='\r\n').writerows(rows)
        data=text.getvalue().encode('utf-8');path=Path(directory)/'case.csv';path.write_bytes((b'\xef\xbb\xbf' if case%7==0 else b'')+data)
        result=subprocess.run([str(out),str(path),str(ord(delimiter))],check=True,capture_output=True,text=True).stdout.splitlines()
        count,columns=map(int,result[0].split());assert count==len(rows) and columns==max(map(len,rows))
        for expected,line in zip(rows,result[1:]):
            parts=line.split(' ');assert int(parts[0])==len(expected)
            assert [bytes.fromhex(v).decode('utf-8') for v in parts[1:]]==expected,(case,expected,line)
(ROOT/'build/lvgl/csv-model-result.json').write_text(json.dumps(dict(status='passed',generated_cases=150,input_sha256={name:hashlib.sha256((PORT/name).read_bytes()).hexdigest() for name in ('csv.c','csv.h','csv-test.c','csv-model-test.py')},checks=['Python csv differential records','UTF-8 and BOM','quoted separators/newlines/quotes','ragged and empty cells','transactional errors','stable numeric sorting','TSV clipboard']),indent=2)+'\n')
print('CSV differential parser: 150 generated cases passed')
