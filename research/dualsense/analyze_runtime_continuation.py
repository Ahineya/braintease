"""Validate saved continuation evidence and summarize it without CPU execution."""
from pathlib import Path
import sys,json,hashlib,collections,struct
sys.path.insert(0,str(Path(__file__).resolve().parent/'analysis/toolchain/python'))
import pefile

HERE=Path(__file__).resolve().parent
ROOT=HERE/'analysis/remoteplay-win'
def load(path):return json.loads(path.read_text())
def evidence(path):
 data=path.read_bytes()
 return {'path':str(path.relative_to(HERE)),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}

def main():
 previous=load(HERE/'runtime-bootstrap-findings.json')
 for item in previous['evidence']:
  if evidence(HERE/item['path'])!=item:raise RuntimeError('Historical bootstrap evidence changed: '+item['path'])
 stages={name:load(ROOT/f'derived/context-continuation-{name}.json') for name in ['imports','module-pointer','module-data']}
 for name in ['runtime.dll','remoteplay.exe']:
  actual=evidence(ROOT/'payload/Data1'/name)['sha256']
  field='source_sha256' if name=='runtime.dll' else 'main_sha256'
  if any(stage[field]!=actual for stage in stages.values()):raise RuntimeError('Input differs from saved trace: '+name)
 if evidence(ROOT/'payload/Data1/preloader.dll')['sha256']!='78f0f88ed59e6d8d26f3f149703d781cdf2b60edc81db59eca310f3d1c3301c7':raise RuntimeError('Unexpected preloader input')
 for name in ['imports','module-pointer']:
  runs=stages[name]['runs']
  if [r['seed'] for r in runs]!=list(range(6)):raise RuntimeError('Six differential seeds required')
  for run in runs:
   expected='0x88' if name=='imports' else '0x20'
   if run['stop']!='unmodeled preloader table field' or run['table_reads'][-1]['offset']!=expected:raise RuntimeError('Unexpected strict boundary')
   if name=='module-pointer' and run['import_cache_after']!=list(stages[name]['api_tokens'].values()):raise RuntimeError('Import-pointer propagation differs')
 module=stages['module-data']['runs'][0]
 catalog=load(ROOT/'derived/zw-export-catalog.json')
 if catalog['best']['count']!=471 or catalog['matching_records']!=471:raise RuntimeError('Expected independently checked catalog missing')
 nt=pefile.PE(str(ROOT/'windows-data/ntdll-19041.207.dll'))
 exports=nt.DIRECTORY_ENTRY_EXPORT
 symbols={nt.get_rva_from_offset(s.name_offset):s.name.decode('ascii') for s in exports.symbols if s.name}
 first=[int(r['va'],16)-0x62000000 for r in module['module_reads'] if r['at']=='0x2028ca969']
 second=[int(r['va'],16)-0x62000000-1 for r in module['module_reads'] if r['at']=='0x2034a65de']
 if len(first)!=exports.struct.NumberOfNames or set(first)!=set(symbols):raise RuntimeError('First walk did not inspect every named export')
 zw={rva for rva,name in symbols.items() if name.startswith('Zw')}
 if set(second)!=zw or len(second)!=len(zw):raise RuntimeError('Second-character reads differ from Zw export set')
 allocations=[int(item['allocation'],16) for item in module['stack_probes']]
 if len(zw)*16 not in allocations:raise RuntimeError('Expected Zw-sized stack allocation not recorded')
 pre=pefile.PE(str(ROOT/'payload/Data1/preloader.dll'))
 imports={e.address:(d.dll.decode(),e.name.decode() if e.name else None) for d in pre.DIRECTORY_ENTRY_IMPORT for e in d.imports}
 if imports[0x180004020]!=('ntdll.dll','MD5Init') or imports[0x1800040b0]!=('ntdll.dll','RtlPcToFileHeader'):raise RuntimeError('Preloader import evidence differs')
 scope=struct.unpack('<5I',pre.get_data(0x833c,20))
 if scope!=(1,0x3546,0x3551,0x34c0,0x3586):raise RuntimeError('Preloader exception scope differs')
 if pre.get_data(0x3546,10)!=bytes.fromhex('b904000000e8f0020000'):raise RuntimeError('Invalid-pointer call differs')
 filter_bytes=pre.get_data(0x34c0,35)
 if filter_bytes!=bytes.fromhex('48b80000000000000000488b4908488b89f800000048898c0230000000b801000000c3'):raise RuntimeError('Exception filter differs')
 md5=next(s for s in exports.symbols if s.name==b'MD5Init')
 if nt.get_data(md5.address,3)!=bytes.fromhex('832100') or md5.forwarder:raise RuntimeError('MD5Init data fixture differs')
 proof={'preloader_source':evidence(ROOT/'payload/Data1/preloader.dll'),'invalid_pointer_call':{'va':'0x180003546','rcx':4,'import':'ntdll.dll!MD5Init'},'exception_scope':[hex(v) for v in scope],'filter':{'va':'0x1800034c0','bytes':filter_bytes.hex(),'context_pointer_offset':'0x8','saved_RIP_offset':'0xf8','return':1},'module_base_lookup':{'va':'0x180003569','import':'ntdll.dll!RtlPcToFileHeader'},'ntdll_MD5Init':{'rva':hex(md5.address),'first_instruction':'and dword ptr [rcx], 0','bytes':'832100'},'interpretation':'Static module-discovery chain using a handled invalid-pointer fault, not an observed native Windows exception or controller crypto operation'}
 (ROOT/'derived/preloader-module-base-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
 files=['emulate_runtime_continue.py','test_runtime_continuation.py','check_runtime_bootstrap.py','analyze_runtime_continuation.py','analyze_runtime_catalog.py','fetch_windows_data.py','windows-data-source.json']
 files += ['analysis/remoteplay-win/derived/'+name for name in ['context-continuation-imports.json','context-continuation-module-pointer.json','context-continuation-module-data.json','context-continuation-mutations.json','continuation-policy-controls.json','preloader-module-base-proof.json','zw-export-catalog.json','context-continuation-module-data-seed0.stack.bin']]
 finding={'date':'2026-10-22','scope':'Offline Windows Remote Play bootstrap research; no native application launch or USB access','inputs':[evidence(ROOT/'payload/Data1'/name) for name in ['runtime.dll','remoteplay.exe','preloader.dll']]+[evidence(ROOT/'windows-data/ntdll-19041.207.dll')],'historical_evidence_verified':len(previous['evidence']),'differential_stages':{name:{'seeds':[r['seed'] for r in stages[name]['runs']],'steps':sorted({r['steps'] for r in stages[name]['runs']}),'boundary_offset':stages[name]['runs'][0]['table_reads'][-1]['offset']} for name in ['imports','module-pointer']},'table_flag_copy':{'source_offset':'0x85','runtime_va':'0x200318078','mutation_control':'zero and one propagate to runtime copy'},'module_discovery':proof,'export_walk':{'module_version':'10.0.19041.207','synthetic_base':'0x62000000','named_exports_examined':len(first),'Zw_prefix_exports':len(second),'stack_allocation_bytes':len(zw)*16,'inference':'Sorted Zw export-address/name-hash catalog; 470 standard stub service IDs match their catalog indices; ZwQuerySystemTime begins with a jump and is not checked by that prefix test','catalog_records':catalog['best']['count'],'independent_hash_matches':catalog['matching_records'],'catalog_index_matches_stub_id':catalog['catalog_index_matches_stub_id'],'catalog_stack_va':catalog['best']['stack_va']},'extended_run':{key:module[key] for key in ['seed','steps','seconds','stop','last_va','body_chain','stack_probes']},'extended_boundary':module.get('blocked_instruction',module.get('memory_fault',module.get('exit_va'))),'main_image_read_count':len(module['main_image_reads']),'module_read_counts_by_pc':dict(collections.Counter(r['at'] for r in module['module_reads'])),'containment_controls':load(ROOT/'derived/continuation-policy-controls.json')['passed'],'unresolved':['Encoded API/syscall globals and catalog consumers','Main EXE unpacking and HID transport callers','Controller firmware decryption, custom code acceptance and RAM execution'],'sources':['https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context','https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_pointers','https://github.com/m417z/winbindex/blob/main/winbindex.js'],'evidence':[evidence(HERE/name) for name in files]}
 (HERE/'runtime-continuation-findings.json').write_text(json.dumps(finding,indent=2)+'\n')
 print(json.dumps({'named_exports':len(first),'Zw_exports':len(second),'steps':module['steps'],'stop':module['stop'],'controls':finding['containment_controls'],'historical_evidence_verified':len(previous['evidence'])}))

if __name__=='__main__':main()
