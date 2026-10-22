"""Offline continuation after the source-attributed preloader table initializer.

Derived from emulate_runtime.py, whose original barrier experiment is preserved.
Writes are recorded and limited to the stack and copied PE data sections. Code,
the main EXE and optional Windows module data remain immutable. CPU queries use
an explicit fixture. Unknown context fields and Windows calls stop. No native
Windows execution or USB access occurs.
"""
from pathlib import Path
import sys,hashlib,json,random,time,collections,struct,argparse
sys.path.insert(0,str(Path(__file__).resolve().parent/'analysis/toolchain/python'))
import pefile,capstone,unicorn
from unicorn import x86_const as x86
root=Path(__file__).resolve().parent/'analysis/remoteplay-win'
raw=(root/'payload/Data1/runtime.dll').read_bytes();digest=hashlib.sha256(raw).hexdigest()
if digest!='ae4ae997261fd9da4dddbba0edea6244bcefe2f24899221f793826b4da497e87':raise RuntimeError('Unexpected source DLL digest')
p=pefile.PE(data=raw);base=p.OPTIONAL_HEADER.ImageBase;image=p.get_memory_mapped_image();image_size=(p.OPTIONAL_HEADER.SizeOfImage+4095)&~4095
main_raw=(root/'payload/Data1/remoteplay.exe').read_bytes();main_digest=hashlib.sha256(main_raw).hexdigest()
if main_digest!='d4e355c18bab988bdc3dc69ef4746d5064983cc7cac699ccda8d46b834291616':raise RuntimeError('Unexpected main EXE input identity')
main_pe=pefile.PE(data=main_raw);main_base=main_pe.OPTIONAL_HEADER.ImageBase;main_image=main_pe.get_memory_mapped_image();main_size=(main_pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
PROCESS=0x50001000
THREAD=0x50000000
STACK=0x70000000;STACK_SIZE=0x100000;MAIN_SP=STACK+STACK_SIZE-0x2000
RANGES=[(0x20082e9bf,0x20082e9d2),(0x203176330,0x203178457),(0x2008c15df,0x2008c15f2),(0x2007c1e29,0x2007c1f46),(0x203972b41,0x203981ac1)]
RANGES += [(0x2006e128f,0x2006e12aa),(0x2008d45c7,0x2008d45c9),(0x2008d45e5,0x2008d45f3),(0x2006da44a,0x2006da456),(0x2008eea36,0x2008eeaf0),(0x2002bf2b0,0x2002bf45c)]
RANGES += [(0x2008d45f3,0x2008d461b),(0x200788ff7,0x20078900a),(0x20075623b,0x200756247),(0x2008519b1,0x2008519c4),(0x200838b3a,0x200838b46)]
RANGES += [(0x20088574d,0x200885768),(0x2008817f9,0x2008818ad),(0x20018ad20,0x20018ad3e),(0x2001c0390,0x2001c0a05)]
RANGES += [(0x2008d461b,0x2008d4630),(0x2006c1a52,0x2006c1a65),(0x20081b040,0x20081b04c),(0x2001972a0,0x2001972a5)]
RANGES += [(0x20081a730,0x20081a74b)]
RANGES += [(0x2002c00b0,0x2002c00fe)] # Reviewed stack-page probe; no Windows call.
RANGES += [(0x2001a0100,0x2001a01a8)] # Reviewed NUL-terminated byte-string length helper.
REVIEWED_RETURNS={0x20081a743:0x2008d4620,0x200885760:0x2008d45f8,0x20088180b:0x2008d4612,0x2006e12a2:0x2008d45c7,0x2008eea48:0x2008d45ea,0x2002bf45b:0x2008d45f1}
REVIEWED_CALLS={0x2008d461b:0x2006c1a52,0x2008d4622:0x20081b040,0x2008d4629:0x2001972a0,0x2008d4614:0x20018ad20,0x2008d45f3:0x200788ff7,0x2008d45fa:0x20075623b,0x2008d4606:0x2008519b1,0x2008d460d:0x200838b3a,0x2008d45e5:0x2006da44a,0x2008d45ec:0x2002bf2b0}
ALLOWED=set('setae cmove cmovb mul jp bt push pushfq pop popfq lea mov movabs movzx movsxd nop and or xor not neg inc dec add sub imul div shl shr sar sal rol ror rcl rcr cmp test sbb adc clc stc jmp je jne jno jae jo jb js jns jbe ja cmovne sete setne'.split())
CONDITIONS='a ae b be e ne g ge l le o no s ns p np'.split()
ALLOWED.update(prefix+condition for prefix in ['j','cmov','set'] for condition in CONDITIONS)
ALLOWED.update('movsx bswap bsf bsr shld shrd movdqu movdqa movups movaps cbw cwde cdqe cwd cdq cqo cmc lahf sahf xchg'.split())
REGS={name:getattr(x86,'UC_X86_REG_'+name.upper()) for name in ['rax','rbx','rcx','rdx','rbp','rsp','rsi','rdi',*[f'r{i}' for i in range(8,16)]]}
MAX_INSNS=5_000_000
TIMEOUT_US=25_000_000
MAX_CHUNKS=1
MAX_BODIES=128
FUNCTION_RANGES={base+e.struct.BeginAddress:base+e.struct.EndAddress for e in p.DIRECTORY_ENTRY_EXCEPTION}

INITIALIZER=0x2010621e1
CONTEXT_GLOBAL=0x200318e88
# Hypothetical SSE2-only CPU, not captured host hardware or API results.
CPU_PROFILE={0:(1,0x68747541,0x444d4163,0x69746e65),1:(0x00000f00,0,0,0x06000001)}
CPU_SITES={0x2002bf2c3,0x2002bf2ea}
DATA_RANGES=[(base+section.VirtualAddress,base+section.VirtualAddress+section.Misc_VirtualSize)
 for section in p.sections if section.Characteristics&0x80000000 and not section.Characteristics&0x20000000]
TABLE_FLAGS={0x80:bytes(4),0x84:bytes(1),0x85:bytes(1)}
# Explicit scenario: crash environment unset, native Windows (not Wine), x64 host.
MAX_DATA_WRITES=256
API_NAMES='MessageBoxW RegGetValueW RtlQueryEnvironmentVariable_U RtlExpandEnvironmentStrings RtlDosPathNameToNtPathName_U_WithStatus RtlCreateProcessParametersEx RtlCreateEnvironment ZwQuerySystemInformation ZwCreateUserProcess ZwSetInformationFile ZwCreateFile ZwCreateThreadEx ZwQueryInformationFile ZwQueryInformationProcess ZwClose ZwQueryDirectoryFile ZwWaitForSingleObject ZwDeleteFile'.split()
API_TOKEN_BASE=0x61000000
FORBIDDEN_REGISTERS={*[f'cr{i}' for i in range(16)],*[f'dr{i}' for i in range(16)],'cs','ss','ds','es','fs','gs'}
TABLE_VALUES={
 0x00:0xa9738c26f6daad73, 0x08:0x019fcad04bee73a1,
 0x10:0x180000000, 0x18:base, 0x30:0x200260410,
 0x58:0, 0x60:0, 0x68:0x1800035d0, 0x70:0x1800035b0,
 0x78:0x1800020a0, 0x118:0x1800034f0, 0x120:0x180002060,
 0x128:0x180003590, 0x130:0x1800037c0, 0x138:0x180001040,
}

def one(seed,api_tokens=False,module_token=False,windows_data=False,capture_stack=False):
 if windows_data and not module_token:raise ValueError('Windows data requires explicit module token')
 allowed_ranges=list(RANGES);body_chain=[];local_calls=[];observed_returns=set()
 rng=random.Random(seed);u=unicorn.Uc(unicorn.UC_ARCH_X86,unicorn.UC_MODE_64)
 u.mem_map(base,image_size,unicorn.UC_PROT_ALL);u.mem_write(base,image);u.mem_protect(base,image_size,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
 for lo,hi in DATA_RANGES:
  u.mem_protect(lo&~0xfff,((hi+4095)&~4095)-(lo&~0xfff),unicorn.UC_PROT_READ|unicorn.UC_PROT_WRITE)
 u.mem_map(THREAD,0x1000,unicorn.UC_PROT_ALL);u.mem_write(THREAD+0x10,struct.pack('<Q',STACK));u.mem_write(THREAD+0x30,struct.pack('<Q',THREAD));u.mem_write(THREAD+0x60,struct.pack('<Q',PROCESS));u.mem_protect(THREAD,0x1000,unicorn.UC_PROT_READ);u.reg_write(x86.UC_X86_REG_GS_BASE,THREAD)
 u.mem_map(PROCESS,0x1000,unicorn.UC_PROT_ALL);u.mem_write(PROCESS+0x10,struct.pack('<Q',main_base));u.mem_protect(PROCESS,0x1000,unicorn.UC_PROT_READ)
 u.mem_map(main_base,main_size,unicorn.UC_PROT_ALL);u.mem_write(main_base,main_image);u.mem_protect(main_base,main_size,unicorn.UC_PROT_READ)
 module_size=0
 if windows_data:
  module_raw=(root/'windows-data/ntdll-19041.207.dll').read_bytes()
  if hashlib.sha256(module_raw).hexdigest()!='96edaf03c5c54e5e0f8892a4944020dc1d4ca8edeb8de3fa1dabf02c61812dda':raise RuntimeError('Unexpected Windows data module')
  module_pe=pefile.PE(data=module_raw);module_size=(module_pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095
  u.mem_map(0x62000000,module_size,unicorn.UC_PROT_ALL);u.mem_write(0x62000000,module_pe.get_memory_mapped_image());u.mem_protect(0x62000000,module_size,unicorn.UC_PROT_READ)
 u.mem_map(STACK,STACK_SIZE,unicorn.UC_PROT_READ|unicorn.UC_PROT_WRITE)
 if seed:u.mem_write(STACK,rng.randbytes(STACK_SIZE))
 initial={name:(rng.getrandbits(64) if seed else 0) for name in REGS}
 initial.update({'rsp':MAIN_SP-8,'rbx':MAIN_SP,'rbp':MAIN_SP+0x80,'rcx':0,'rdx':0x25665cd7,'r8':0,'r9':0x60000100+seed*0x1000})
 table=initial['r9'];table_page=table&~0xfff
 u.mem_map(table_page,0x1000,unicorn.UC_PROT_ALL)
 valid_table_bytes=set()
 values=dict(TABLE_VALUES)
 if module_token:values[0x20]=0x62000000
 if api_tokens:values.update({0x88+i*8:API_TOKEN_BASE+i*0x100 for i in range(len(API_NAMES))})
 for offset,value in values.items():
  u.mem_write(table+offset,struct.pack('<Q',value));valid_table_bytes.update(range(offset,offset+8))
 for offset,data in TABLE_FLAGS.items():
  u.mem_write(table+offset,data);valid_table_bytes.update(range(offset,offset+len(data)))
 u.mem_protect(table_page,0x1000,unicorn.UC_PROT_READ)
 for name,v in initial.items():u.reg_write(REGS[name],v)
 u.reg_write(x86.UC_X86_REG_FS,0);u.reg_write(x86.UC_X86_REG_FS_BASE,0)
 u.reg_write(x86.UC_X86_REG_EFLAGS,0x202)
 u.mem_write(MAIN_SP-8,struct.pack('<Q',0x2008d45c7));u.mem_write(MAIN_SP+0x748,struct.pack('<Q',initial['rbp']))
 result={'seed':seed,'synthetic_api_tokens':api_tokens,'synthetic_module_token':module_token,'windows_data':windows_data,'module_reads':[],'initial_registers':{k:hex(v) for k,v in initial.items()},'steps':0,'image_reads':[],'thread_reads':[],'process_reads':[],'main_image_reads':[],'stack_probes':[],'data_writes':[],'cpu_queries':[],'table_reads':[],'modeled_stores':[],'stack_writes':0,'stop':None,'last_va':None,'mnemonics':{},'trace_tail':[]}
 cache={};history=collections.deque(maxlen=20);mnemonics=collections.Counter();seen_reads=set()
 def code(uc,a,size,_):
  result['steps']+=1
  if a==0x2002c00b0:result['stack_probes'].append({'at':hex(a),'step':result['steps'],'allocation':hex(uc.reg_read(REGS['rax'])),'rsp':hex(uc.reg_read(REGS['rsp']))})
  if a in CPU_SITES:
   leaf=uc.reg_read(REGS['rax'])&0xffffffff;subleaf=uc.reg_read(REGS['rcx'])&0xffffffff
   if bytes(uc.mem_read(a,size))!=bytes.fromhex('0fa2') or leaf not in CPU_PROFILE or subleaf!=0:
    result['stop']='unmodeled CPU query';uc.emu_stop();return
   values=CPU_PROFILE[leaf]
   for name,value in zip(['rax','rbx','rcx','rdx'],values):uc.reg_write(REGS[name],value)
   result['cpu_queries'].append({'at':hex(a),'leaf':leaf,'subleaf':subleaf,'outputs':[hex(v) for v in values]})
   uc.reg_write(x86.UC_X86_REG_RIP,a+size);return
  if a==INITIALIZER:
   target=uc.reg_read(REGS['rax']);value=uc.reg_read(REGS['r13'])
   if bytes(uc.mem_read(a,size))!=bytes.fromhex('4c8928') or target!=CONTEXT_GLOBAL or value!=table or result['modeled_stores']:
    result['stop']='initializer policy mismatch';uc.emu_stop();return
   before=bytes(uc.mem_read(target,8)).hex()
   result['modeled_stores'].append({'at':hex(a),'va':hex(target),'size':8,'value':hex(value),'before':before,'policy':'validated original MOV; emulator-owned data section only'})
  if result['steps']>MAX_INSNS:result['stop']='instruction budget';uc.emu_stop();return
  if a not in cache:
   if not any(lo<=a<hi for lo,hi in allowed_ranges):
    end=FUNCTION_RANGES.get(a)
    if end is not None and 0<end-a<=0x20000 and len(body_chain)<MAX_BODIES:
     allowed_ranges.append((a,end));body_chain.append({'start':hex(a),'end':hex(end),'at_step':result['steps']})
    elif size==5 and bytes(uc.mem_read(a,1))==b'\xe9' and a+5+struct.unpack('<i',uc.mem_read(a+1,4))[0] in FUNCTION_RANGES and len(body_chain)<MAX_BODIES:
     allowed_ranges.append((a,a+5));body_chain.append({'start':hex(a),'end':hex(a+5),'at_step':result['steps'],'kind':'observed direct jump to PE function start'})
    else:result['stop']='helper exit before target instruction';result['exit_va']=hex(a);uc.emu_stop();return
   ins=next(md.disasm(bytes(uc.mem_read(a,size)),a,count=1),None)
   if ins is None or ins.size!=size:
    if a==0x200813f40 and size==2 and bytes(uc.mem_read(a,size))==bytes.fromhex('63d1'):
     import iced_x86
     decoded=iced_x86.Decoder(64,bytes.fromhex('63d1'),ip=a).decode()
     if decoded.code!=iced_x86.Code.MOVSXD_R32_RM32 or decoded.op0_register!=iced_x86.Register.EDX or decoded.op1_register!=iced_x86.Register.ECX or decoded.len!=2:raise RuntimeError('Independent decoder disagrees on reviewed MOVSXD')
     cache[a]=('movsxd','edx, ecx',False)
    else:result['stop']='decoder disagreement';uc.emu_stop();return
   else:
    restricted=any(operand.strip() in FORBIDDEN_REGISTERS for operand in ins.op_str.split(','))
    cache[a]=(ins.mnemonic,ins.op_str,restricted)
  name,ops,restricted=cache[a]
  if restricted and not (a==0x200810aff and bytes(uc.mem_read(a,size))==bytes.fromhex('8ce2') and ops=='edx, fs'):
   result['stop']='blocked system-register access';result['blocked_instruction']={'va':hex(a),'asm':name+' '+ops};uc.emu_stop();return
  local_trampoline=False
  if name=='call':
   ins=next(md.disasm(bytes(uc.mem_read(a,size)),a,count=1))
   if ins.op_str.startswith('0x'):
    target=int(ins.op_str,16)
    if any(lo<=target<hi for lo,hi in allowed_ranges) and bytes(uc.mem_read(target,1))==b'\xc3':
     local_trampoline=True;local_calls.append({'call':a,'target':target,'return':a+size})
  if name=='call' and ops.startswith('0x'):
   target=int(ops,16);end=FUNCTION_RANGES.get(target)
   if any(lo<=target<hi for lo,hi in allowed_ranges) or (end is not None and 0<end-target<=0x20000):
    local_trampoline=True;observed_returns.add(a+size)
  if name=='call' and a in REVIEWED_CALLS:
   local_trampoline=ops==hex(REVIEWED_CALLS[a])
  if name=='ret' and a in REVIEWED_RETURNS:
   return_va=struct.unpack('<Q',uc.mem_read(uc.reg_read(REGS['rsp']),8))[0]
   local_trampoline=return_va==REVIEWED_RETURNS[a]
  elif name=='ret' and struct.unpack('<Q',uc.mem_read(uc.reg_read(REGS['rsp']),8))[0] in observed_returns:
   local_trampoline=True
  elif name=='ret' and local_calls:
   stack_pointer=uc.reg_read(REGS['rsp']);return_va=struct.unpack('<Q',uc.mem_read(stack_pointer,8))[0]
   local_trampoline=local_calls[-1]['target']==a and local_calls[-1]['return']==return_va
   if local_trampoline:local_calls.pop()
  if name not in ALLOWED and not local_trampoline:result['stop']='blocked instruction '+name;result['blocked_instruction']={'va':hex(a),'asm':name+' '+ops};uc.emu_stop();return
  result['last_va']=hex(a);mnemonics[name]+=1;history.append({'va':hex(a),'asm':name+' '+ops})
 def read(uc,access,a,size,value,_):
  if module_size and 0x62000000<=a and a+size<=0x62000000+module_size:
   if len(result['module_reads'])>=32768:result['stop']='module read log budget';uc.emu_stop();return
   result['module_reads'].append({'at':result['last_va'],'va':hex(a),'size':size,'bytes':bytes(uc.mem_read(a,size)).hex()});return
  if table_page<=a<table_page+0x1000:
   offset=a-table
   known=all(i in valid_table_bytes for i in range(offset,offset+size))
   result['table_reads'].append({'at':result['last_va'],'offset':hex(offset),'size':size,'known':known,'bytes':bytes(uc.mem_read(a,size)).hex() if known else None})
   if not known:result['stop']='unmodeled preloader table field';uc.emu_stop()
   return
  if THREAD<=a<THREAD+0x1000:
   result['thread_reads'].append({'at':result['last_va'],'va':hex(a),'size':size})
   if a not in [THREAD+0x10,THREAD+0x30,THREAD+0x60] or size!=8:
    result['stop']='unmodeled synthetic thread field';uc.emu_stop()
   return
  if PROCESS<=a<PROCESS+0x1000:
   result['process_reads'].append({'at':result['last_va'],'va':hex(a),'size':size})
   if a!=PROCESS+0x10 or size!=8:result['stop']='unmodeled synthetic process field';uc.emu_stop()
   return
  if main_base<=a and a+size<=main_base+main_size:
   if len(result['main_image_reads'])>=1024:result['stop']='main image read log budget';uc.emu_stop();return
   result['main_image_reads'].append({'at':result['last_va'],'va':hex(a),'size':size,'bytes':bytes(uc.mem_read(a,size)).hex()});return
  if base<=a and a+size<=base+image_size and (a,size) not in seen_reads:
   seen_reads.add((a,size))
   if len(result['image_reads'])>=1024:result['stop']='image read log budget';uc.emu_stop();return
   result['image_reads'].append({'at':result['last_va'],'va':hex(a),'size':size,'bytes':bytes(uc.mem_read(a,size)).hex()})
 def write(uc,access,a,size,value,_):
  if any(lo<=a and a+size<=hi for lo,hi in DATA_RANGES):
   if len(result['data_writes'])>=MAX_DATA_WRITES:
    result['stop']='data write log budget';uc.emu_stop();return
   result['data_writes'].append({'at':result['last_va'],'va':hex(a),'size':size,'value_low64' if size>8 else 'value':hex(value&((1<<(min(size,8)*8))-1)),'before':bytes(uc.mem_read(a,size)).hex()})
   return
  if not STACK<=a or a+size>STACK+STACK_SIZE:result['stop']='blocked non-stack write';result['blocked_write']={'at':result['last_va'],'va':hex(a),'size':size,'value':hex(value&((1<<(size*8))-1))};uc.emu_stop();return
  result['stack_writes']+=1
 def invalid(uc,access,a,size,value,_):
  if result['stop'] is None:result['stop']='invalid or protected virtual-memory access'
  result['memory_fault']={'access':access,'va':hex(a),'size':size};return False
 u.hook_add(unicorn.UC_HOOK_CODE,code);u.hook_add(unicorn.UC_HOOK_MEM_READ,read);u.hook_add(unicorn.UC_HOOK_MEM_WRITE,write);u.hook_add(unicorn.UC_HOOK_MEM_INVALID,invalid)
 started=time.monotonic();result['chunks']=[]
 for chunk in range(MAX_CHUNKS):
  start=0x2008c15df if chunk==0 else u.reg_read(x86.UC_X86_REG_RIP)
  try:u.emu_start(start,0,count=max(1,MAX_INSNS-result['steps']+1),timeout=TIMEOUT_US)
  except unicorn.UcError as e:
   result['emulator_error']=str(e)
   if result['stop'] is None:result['stop']='emulator error'
  result['chunks'].append({'steps':result['steps'],'next_rip':hex(u.reg_read(x86.UC_X86_REG_RIP)),'seconds':round(time.monotonic()-started,6)})
  if result['stop'] is not None:break
  if MAX_CHUNKS>1:print(json.dumps({'progress':result['chunks'][-1],'seed':seed}),flush=True)
 if result['stop'] is None:result['stop']='emulator time/count bound'
 result['import_cache_after']=[hex(struct.unpack('<Q',u.mem_read(0x200318090+i*8,8))[0]) for i in range(len(API_NAMES))]
 for item in result['data_writes']:item['final_bytes']=bytes(u.mem_read(int(item['va'],16),item['size'])).hex()
 for item in result['modeled_stores']:item['final_bytes']=bytes(u.mem_read(int(item['va'],16),item['size'])).hex()
 result['body_chain']=body_chain;result['admitted_ranges']=[[hex(lo),hex(hi)] for lo,hi in allowed_ranges];result['seconds']=round(time.monotonic()-started,6);result['registers']={name:hex(u.reg_read(reg)) for name,reg in REGS.items()};result['mnemonics']=dict(mnemonics);result['trace_tail']=list(history)
 frame=MAIN_SP-0x230
 result['saved_frame_after']={hex(off):hex(struct.unpack('<Q',u.mem_read(frame+off,8))[0]) for off in range(0,0x230,8)}
 if capture_stack:
  path=root/f'derived/context-continuation-module-data-seed{seed}.stack.bin'
  snapshot=bytes(u.mem_read(STACK,STACK_SIZE));path.write_bytes(snapshot)
  result['stack_snapshot']={'path':str(path.relative_to(root)),'base':hex(STACK),'size':len(snapshot),'sha256':hashlib.sha256(snapshot).hexdigest(),'contents':'Synthetic emulator stack only; not host memory'}
 return result

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--stage',choices=['imports','module-pointer','module-data'],default='imports')
 parser.add_argument('--seeds',default='0,1,2,3,4,5')
 parser.add_argument('--instructions',type=int,default=5_000_000)
 parser.add_argument('--timeout-ms',type=int,default=25000)
 parser.add_argument('--chunks',type=int,default=1)
 args=parser.parse_args()
 global MAX_INSNS,TIMEOUT_US,MAX_CHUNKS
 if not 1<=args.instructions<=100_000_000 or not 1<=args.timeout_ms<=55000 or not 1<=args.chunks<=8:parser.error('Budget outside permitted bounds')
 MAX_INSNS=args.instructions;TIMEOUT_US=args.timeout_ms*1000;MAX_CHUNKS=args.chunks
 seeds=[int(s) for s in args.seeds.split(',')]
 if not seeds or len(seeds)>6 or any(not 0<=s<=5 for s in seeds):parser.error('Choose at most six seeds from 0 through 5')
 results=[]
 for seed in seeds:
  result=one(seed,api_tokens=args.stage!='imports',module_token=args.stage=='module-data',windows_data=args.stage=='module-data',capture_stack=args.stage=='module-data')
  if len(result['modeled_stores'])!=1:raise RuntimeError('Initializer not reproduced')
  if args.stage=='imports':
   if result['stop']!='unmodeled preloader table field' or result['table_reads'][-1]['offset']!='0x88':raise RuntimeError('Unexpected import boundary')
  elif result['import_cache_after']!=[hex(API_TOKEN_BASE+i*0x100) for i in range(len(API_NAMES))]:raise RuntimeError('Import-pointer copy differs from table')
  results.append(result)
  print(json.dumps({k:v for k,v in result.items() if k in ['seed','steps','stop','last_va','exit_va','memory_fault','blocked_instruction','seconds']}),flush=True)
 report={'source_sha256':digest,'main_sha256':main_digest,'toolchain':{'unicorn':unicorn.__version__,'capstone':capstone.__version__,'pefile':pefile.__version__},'stage':args.stage,'mode':'offline emulation; immutable code; recorded copied-data writes; no native Windows execution or device access','table_values':{hex(k):hex(v) for k,v in TABLE_VALUES.items()},'table_flags':{hex(k):v.hex() for k,v in TABLE_FLAGS.items()},'api_tokens':{name:hex(API_TOKEN_BASE+i*0x100) for i,name in enumerate(API_NAMES)} if args.stage!='imports' else None,'cpu_fixture':{str(k):list(v) for k,v in CPU_PROFILE.items()},'data_ranges':[[hex(a),hex(b)] for a,b in DATA_RANGES],'instruction_limit':MAX_INSNS,'timeout_us_per_chunk':TIMEOUT_US,'chunk_limit':MAX_CHUNKS,'function_admission_limit':MAX_BODIES,'unknown_table_fields':'hard stop; no invented API return values','windows_module':'windows-data/ntdll-19041.207.dll, read-only NX, mapped at synthetic address 0x62000000 without applying relocations' if args.stage=='module-data' else None,'runs':results}
 (root/f'derived/context-continuation-{args.stage}.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
