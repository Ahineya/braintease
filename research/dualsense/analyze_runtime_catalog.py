"""Carve candidate Zw records from the saved synthetic stack, never host memory.

The independently transcribed string hash covers 4..128 ASCII bytes, the
relevant branches of original runtime function 0x2002c5440. Unsupported lengths
fail explicitly. Stored records are checked against the actual DLL exports.
"""
from pathlib import Path
import sys,json,hashlib,struct
sys.path.insert(0,str(Path(__file__).resolve().parent/'analysis/toolchain/python'))
import pefile
ROOT=Path(__file__).resolve().parent/'analysis/remoteplay-win'
MASK=(1<<64)-1
def rol(x,n):return ((x<<n)|(x>>(64-n)))&MASK
def fold(a,b):
 product=a*b
 return ((product&MASK)^(product>>64))&MASK
def name_hash(data):
 n=len(data)
 if not 4<=n<=128:raise ValueError('Only reviewed 4..128-byte hash branches are implemented')
 def word(offset):return int.from_bytes(data[offset:offset+8],'little')
 if n<=8:
  x=((int.from_bytes(data[:4],'little')<<32)|int.from_bytes(data[-4:],'little'))^0x595aec68add1b558
  x=((rol(x,49)^rol(x,24)^x)*0x9fb21c651e98df25)&MASK
  x=((((x>>35)+n)^x)*0x9fb21c651e98df25)&MASK
  return x^(x>>28)
 if n<=16:
  a=word(0)^0x8f825190025d5a03;b=word(n-8)^0x8756de26f17b31f0
  swapped=int.from_bytes(a.to_bytes(8,'little'),'big')
  acc=(n+b+swapped+fold(a,b))&MASK
 else:
  acc=(n*0x9e3779b185ebca87)&MASK
  if n>=97:
   acc+=fold(word(0x38)^0xf54f2db3afa0bcaf,word(0x30)^0x67347af757921af2)
   acc+=fold(word(n-0x38)^0x3c739ac5b1637f7e,word(n-0x40)^0x595992e163fb71d4)
  if n>=65:
   acc+=fold(word(0x28)^0x7b2e7507734ab03e,word(0x20)^0xf300a1a5d36d4886)
   acc+=fold(word(n-0x28)^0xb0ad00952ed3f8ee,word(n-0x30)^0x76f8ebb6af6384bb)
  if n>=33:
   acc+=fold(word(0x18)^0xf97321b865b53a38,word(0x10)^0xa0e59ee067019a15)
   acc+=fold(word(n-0x18)^0x24265c6dce751596,word(n-0x20)^0xb624220b8c612902)
  acc+=fold(word(8)^0xf4ad43e31465e132,word(0)^0xe64b823751881f02)
  acc+=fold(word(n-8)^0xf767d5a38c892028,word(n-16)^0x03976e980188f528)
  acc&=MASK
 acc=((acc^(acc>>37))*0x165667919e3779f9)&MASK
 return acc^(acc>>32)

def main():
 report=json.loads((ROOT/'derived/context-continuation-module-data.json').read_text())
 run=report['runs'][0];snapshot=run['stack_snapshot'];stack=(ROOT/snapshot['path']).read_bytes()
 if len(stack)!=snapshot['size'] or hashlib.sha256(stack).hexdigest()!=snapshot['sha256']:raise RuntimeError('Stack snapshot differs')
 source=ROOT/'windows-data/ntdll-19041.207.dll'
 if hashlib.sha256(source.read_bytes()).hexdigest()!='96edaf03c5c54e5e0f8892a4944020dc1d4ca8edeb8de3fa1dabf02c61812dda':raise RuntimeError('Windows data fixture differs')
 runtime=(ROOT/'payload/Data1/runtime.dll').read_bytes()
 if hashlib.sha256(runtime).hexdigest()!='ae4ae997261fd9da4dddbba0edea6244bcefe2f24899221f793826b4da497e87':raise RuntimeError('Hash-routine source differs')
 pe=pefile.PE(str(source));by_address={}
 for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols:
  if symbol.name and symbol.name.startswith(b'Zw'):
   by_address.setdefault(0x62000000+symbol.address,[]).append(symbol.name.decode('ascii'))
 candidates=[]
 for offset in range(0,len(stack)-15,8):
  first,second=struct.unpack_from('<QQ',stack,offset)
  if first not in by_address:continue
  if offset>=16 and struct.unpack_from('<Q',stack,offset-16)[0] in by_address:continue
  records=[];position=offset
  while position+16<=len(stack):
   address,value=struct.unpack_from('<QQ',stack,position)
   if address not in by_address:break
   names=by_address[address];matches=[name for name in names if name_hash(name.encode())==value]
   records.append({'address':hex(address),'export_names':names,'stored_hash':hex(value),'hash_matches':matches})
   position+=16
  candidates.append({'stack_va':hex(int(snapshot['base'],16)+offset),'count':len(records),'records':records})
 candidates.sort(key=lambda c:c['count'],reverse=True)
 if not candidates:raise RuntimeError('No candidate records in synthetic stack')
 best=candidates[0]
 for index,record in enumerate(best['records']):
  stub=pe.get_data(int(record['address'],16)-0x62000000,8)
  record['catalog_index']=index
  record['stub_first_bytes']=stub.hex()
  record['stub_service_id']=struct.unpack_from('<I',stub,4)[0] if stub[:4]==bytes.fromhex('4c8bd1b8') else None
 result={'scope':'Offline analysis of synthetic stack bytes only','snapshot':snapshot,'source_hash_function':'runtime.dll preferred VA 0x2002c5440','hash_branch_lengths':[4,128],'candidate_count':len(candidates),'best':best,'matching_records':sum(bool(r['hash_matches']) for r in best['records']),'sorted_by_address_then_hash':[(int(r['address'],16),int(r['stored_hash'],16)) for r in best['records']]==sorted((int(r['address'],16),int(r['stored_hash'],16)) for r in best['records'])}
 result['catalog_index_matches_stub_id']=sum(r['catalog_index']==r['stub_service_id'] for r in best['records'])
 result['nonmatching_stub_checks']=[r for r in best['records'] if r['catalog_index']!=r['stub_service_id']]
 expected={(hex(address),name) for address,names in by_address.items() for name in names}
 recovered={(record['address'],name) for record in best['records'] for name in record['hash_matches']}
 result['complete_export_set']=expected==recovered
 result['observed_name_lengths']=sorted({len(name) for _,name in recovered})
 if best['count']!=sum(len(v) for v in by_address.values()) or result['matching_records']!=best['count'] or not result['sorted_by_address_then_hash'] or not result['complete_export_set']:raise RuntimeError('Complete sorted catalog with independently matching hashes not recovered')
 (ROOT/'derived/zw-export-catalog.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps({k:v for k,v in result.items() if k!='best'}));print(json.dumps({'best_stack_va':best['stack_va'],'records':best['count']}))

if __name__=='__main__':main()
