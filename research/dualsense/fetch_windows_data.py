"""Fetch the pinned Microsoft PE data fixture, without loading or executing it."""
from pathlib import Path
import argparse,hashlib,json,urllib.request

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--verify-offline',action='store_true')
 args=parser.parse_args()
 root=Path(__file__).resolve().parent
 source=json.loads((root/'windows-data-source.json').read_text())
 path=root/source['file']
 if path.exists():data=path.read_bytes()
 elif args.verify_offline:raise FileNotFoundError(path)
 else:
  with urllib.request.urlopen(source['url'],timeout=40) as response:
   data=response.read(source['size']+1)
  if len(data)!=source['size'] or hashlib.sha256(data).hexdigest()!=source['sha256']:
   raise RuntimeError('Downloaded Windows data fixture differs from pin')
  path.parent.mkdir(parents=True,exist_ok=True)
  with path.open('xb') as output:output.write(data)
  path.chmod(0o444)
 if len(data)!=source['size'] or hashlib.sha256(data).hexdigest()!=source['sha256']:
  raise RuntimeError('Local Windows data fixture differs from pin')
 print(json.dumps({'verified':str(path),'size':len(data),'sha256':source['sha256']}))

if __name__=='__main__':main()
