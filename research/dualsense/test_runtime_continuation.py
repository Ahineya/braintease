"""Containment controls for the continuation policy; only synthetic code runs."""
import json,struct
import emulate_runtime_continue as m

entry=0x2008c15df
original_ctor=m.unicorn.Uc
engines=[]
chunk_count=None
def capture(*args,**kwargs):
 engine=original_ctor(*args,**kwargs);engines.append(engine)
 if chunk_count:
  original_start=engine.emu_start
  def bounded_start(*args,**kwargs):
   kwargs['count']=min(kwargs['count'],chunk_count)
   return original_start(*args,**kwargs)
  engine.emu_start=bounded_start
 return engine
m.unicorn.Uc=capture
m.FUNCTION_RANGES={};m.MAX_INSNS=1000;m.image_size=0x900000
m.main_size=0x1000;m.main_image=bytes(0x1000)
m.DATA_RANGES=[(m.base+0x318000,m.base+0x319000)]
m.REVIEWED_CALLS={};m.REVIEWED_RETURNS={}
def run(code,extra=None,**options):
 engines.clear();image=bytearray(m.image_size);offset=entry-m.base
 image[offset:offset+len(code)]=code;m.image=bytes(image)
 m.RANGES=[(entry,entry+len(code))]
 if extra:
  for address,data in extra.items():
   image[address-m.base:address-m.base+len(data)]=data;m.RANGES.append((address,address+len(data)))
  m.image=bytes(image)
 result=m.one(0,**options)
 return result,engines[-1]
def mov_rax(value):return b'\x48\xb8'+struct.pack('<Q',value)
checks=[]
def checked(description,condition):
 assert condition,description
 checks.append(description)
r,u=run(mov_rax(42)+bytes.fromhex('0f05'))
checked('SYSCALL stops before execution',r['stop']=='blocked instruction syscall' and int(r['registers']['rax'],16)==42)
r,u=run(bytes.fromhex('0f20c00f05'))
checked('MOV from CR0 is blocked',r['stop']=='blocked system-register access')
r,u=run(bytes.fromhex('8ce20f05'))
checked('Unreviewed segment-selector reads stop',r['stop']=='blocked system-register access')
r,u=run(bytes.fromhex('0fa2'))
checked('CPUID outside fixture sites stops',r['stop']=='blocked instruction cpuid')
r,u=run(mov_rax(m.base+0x1000)+bytes.fromhex('48c70034120000'))
checked('Code copy cannot be written',u.mem_read(m.base+0x1000,8)==bytes(8) and r['stop']=='blocked non-stack write')
address=m.base+0x318100
r,u=run(mov_rax(address)+bytes.fromhex('48c700341200000f05'))
checked('Copied PE data writes are recorded',u.mem_read(address,8)==struct.pack('<Q',0x1234) and len(r['data_writes'])==1 and r['data_writes'][0]['final_bytes']=='3412000000000000')
r,u=run(mov_rax(address)+bytes.fromhex('ffe0'))
checked('Writable data is not executable','FETCH_PROT' in r.get('emulator_error',''))
r,u=run(mov_rax(0x60000120)+bytes.fromhex('488b000f05'))
checked('Unknown table fields stop without a fabricated value',r['stop']=='unmodeled preloader table field' and r['table_reads'][-1]['known'] is False)
r,u=run(mov_rax(0x60000188)+bytes.fromhex('488b000f05'),api_tokens=True)
checked('Named API tokens propagate only as data',int(r['registers']['rax'],16)==m.API_TOKEN_BASE and r['stop']=='blocked instruction syscall')
r,u=run(mov_rax(m.API_TOKEN_BASE)+bytes.fromhex('ffd0'),api_tokens=True)
checked('API-token CALL stops before stack mutation',r['stop']=='blocked instruction call' and int(r['registers']['rsp'],16)==m.MAIN_SP-8)
r,u=run(bytes.fromhex('c3'))
checked('Unobserved RET is blocked',r['stop']=='blocked instruction ret')
r,u=run(bytes.fromhex('e8020000000f05c3'))
checked('Observed local CALL/RET preserves stack balance',r['stop']=='blocked instruction syscall' and int(r['registers']['rsp'],16)==m.MAIN_SP-8)
r,u=run(bytes.fromhex('ebfe'))
checked('Instruction budget bounds loops',r['steps']==m.MAX_INSNS+1 and r['stop']=='instruction budget')
r,u=run(bytes.fromhex('65488b0425100000000f05'))
checked('Synthetic stack-limit cell matches mapped stack',int(r['registers']['rax'],16)==m.STACK and r['stop']=='blocked instruction syscall')
r,u=run(bytes.fromhex('65488b0425380000000f05'))
checked('Other synthetic thread fields remain unknown',r['stop']=='unmodeled synthetic thread field')
r,u=run(mov_rax(m.main_base)+bytes.fromhex('ffe0'))
checked('Main EXE is non-executable','FETCH_PROT' in r.get('emulator_error',''))
saved_initializer=m.INITIALIZER;m.INITIALIZER=entry+20
code=mov_rax(m.CONTEXT_GLOBAL)+b'\x49\xbd'+struct.pack('<Q',0x60000100)+bytes.fromhex('4c89280f05')
r,u=run(code)
checked('Validated initializer executes original MOV and preserves flags',u.mem_read(m.CONTEXT_GLOBAL,8)==struct.pack('<Q',0x60000100) and len(r['modeled_stores'])==1 and u.reg_read(m.x86.UC_X86_REG_EFLAGS)==0x202)
r,u=run(code[:10]+b'\x49\xbd'+struct.pack('<Q',7)+code[20:])
checked('Initializer mismatches stop before storing',r['stop']=='initializer policy mismatch' and u.mem_read(m.CONTEXT_GLOBAL,8)==bytes(8))
m.INITIALIZER=saved_initializer
r,u=run(mov_rax(0x62000000)+bytes.fromhex('ffe0'),api_tokens=True,module_token=True,windows_data=True)
checked('Microsoft module data is non-executable','FETCH_PROT' in r.get('emulator_error',''))
chunk_count=2;m.MAX_CHUNKS=3
r,u=run(mov_rax(1)+bytes.fromhex('48ffc048ffc00f05'))
checked('Chunk continuation preserves the same CPU state',r['stop']=='blocked instruction syscall' and int(r['registers']['rax'],16)==3 and len(r['chunks'])==2)
m.MAX_CHUNKS=1;chunk_count=None
r,u=run(bytes.fromhex('b9ffffffff')+mov_rax(0x200813f40)+bytes.fromhex('ffe0'),extra={0x200813f40:bytes.fromhex('63d10f05')})
checked('Reviewed MOVSXD decoder exception executes with a 32-bit destination',r['stop']=='blocked instruction syscall' and int(r['registers']['rdx'],16)==0xffffffff)
r,u=run(mov_rax(address)+bytes.fromhex('48c700341200000f10000f1140100f05'))
checked('MOVUPS copies the same sixteen bytes within synthetic data',r['stop']=='blocked instruction syscall' and u.mem_read(address+16,16)==struct.pack('<Q',0x1234)+bytes(8))
m.unicorn.Uc=original_ctor
out={'fixture_source':'Synthetic instruction sequences only; Sony and Microsoft bytes are never executed in these controls','passed':len(checks),'checks':checks}
(m.root/'derived/continuation-policy-controls.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
