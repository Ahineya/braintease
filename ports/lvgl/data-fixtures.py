"""Synthetic CSV files only; no normal OS image or host document is read."""
from pathlib import Path
import csv
ROOT=Path(__file__).resolve().parents[2]
def prepare():
    folder=ROOT/'build/lvgl/data-fixtures';folder.mkdir(exist_ok=True)
    (folder/'sample.csv').write_bytes((ROOT/'ports/lvgl/examples/measurements.csv').read_bytes())
    (folder/'sort.csv').write_text('name,value,note\nTen,10,"with, comma"\nTwo,2,"two\nlines"\nMissing,,blank\nEqual,2,stable\nNegative,-5,lowest\n',encoding='utf-8')
    (folder/'table.tsv').write_text('city\treading\nКиїв\t12.5\nЛьвів\t8.0\n',encoding='utf-8')
    (folder/'semi.csv').write_text('city;reading\nKyiv;12\nLviv;8\n')
    (folder/'bad.csv').write_text('a,b\n1,"unclosed\n')
    (folder/'bad-utf8.csv').write_bytes(b'a,b\n\xff,2\n')
    (folder/'empty.csv').write_text('')
    with (folder/'large.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow(['id','value','name'])
        for i in range(100000):writer.writerow([i,100000-i,f'Sample {i:06}'])
    with (folder/'wide.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow([f'Column {i+1}' for i in range(32)])
        for row in range(200):writer.writerow([f'R{row+1}C{col+1}' for col in range(32)])
    files=[dict(name=str(p).encode().decode('latin-1'),path=str(p),mode=0o100644) for p in sorted(folder.glob('*'))]
    return folder,files
if __name__=='__main__':print(prepare()[0])
