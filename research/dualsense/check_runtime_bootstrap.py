"""Differential checks of source-backed table fields in the offline trace."""
import json
import emulate_runtime_continue as m

def main():
 out=[]
 for label,offset,value in [('bad-first-magic',0,0),('bad-second-magic',8,0),('architecture-flag-one',0x85,1)]:
  saved_values=dict(m.TABLE_VALUES);saved_flags=dict(m.TABLE_FLAGS)
  try:
   if offset==0x85:m.TABLE_FLAGS[offset]=bytes([value])
   else:m.TABLE_VALUES[offset]=value
   result=m.one(0,api_tokens=True)
   if offset==0x85:
    writes=[item for item in result['data_writes'] if item['va']=='0x200318078']
    if len(writes)!=1 or writes[0]['value']!='0x1':raise RuntimeError('Architecture flag did not propagate')
    if result['stop']!='unmodeled preloader table field' or result['table_reads'][-1]['offset']!='0x20':raise RuntimeError('Unexpected post-copy boundary')
   elif result['stop']!='helper exit before target instruction' or result['exit_va']!='0x2008dd9dc':
    raise RuntimeError('Altered table magic did not select the observed alternate path')
   out.append({'mutation':label,'offset':hex(offset),'value':value,'run':result})
   print(json.dumps({'mutation':label,'steps':result['steps'],'stop':result['stop']}),flush=True)
  finally:m.TABLE_VALUES=saved_values;m.TABLE_FLAGS=saved_flags
 (m.root/'derived/context-continuation-mutations.json').write_text(json.dumps(out,indent=2)+'\n')

if __name__=='__main__':main()
