"""Bounded, offline x86-64 helper emulation with no native or device execution.

Copied PE bytes, a synthetic stack and three documented/inferred pointer cells
are mapped. The runtime copy is RX; the main image is non-executable read-only data;
only stack memory may be written. Code leaves the explicitly admitted helper
ranges before the next instruction executes. Only checked local CALL-to-RET trampolines are admitted. No syscall, CPU-query, I/O
or other system instruction is admitted. Observations are not universal proofs.
"""
from pathlib import Path
import sys,hashlib,json,random,time,collections,struct
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
ALLOWED=set('setae cmove cmovb mul jp bt push pushfq pop popfq lea mov movabs movzx movsxd nop and or xor not neg inc dec add sub imul div shl shr sar sal rol ror rcl rcr cmp test sbb adc clc stc jmp je jne jno jae jo jb js jns jbe ja cmovne sete setne'.split())
CONDITIONS='a ae b be e ne g ge l le o no s ns p np'.split()
ALLOWED.update(prefix+condition for prefix in ['j','cmov','set'] for condition in CONDITIONS)
ALLOWED.update('movsx bswap bsf bsr shld shrd'.split())
REGS={name:getattr(x86,'UC_X86_REG_'+name.upper()) for name in ['rax','rbx','rcx','rdx','rbp','rsp','rsi','rdi',*[f'r{i}' for i in range(8,16)]]}
MAX_INSNS=250000
MAX_BODIES=12
FUNCTION_RANGES={base+e.struct.BeginAddress:base+e.struct.EndAddress for e in p.DIRECTORY_ENTRY_EXCEPTION}

def one(seed):
 allowed_ranges=list(RANGES);body_chain=[];local_calls=[]
 rng=random.Random(seed);u=unicorn.Uc(unicorn.UC_ARCH_X86,unicorn.UC_MODE_64)
 u.mem_map(base,image_size,unicorn.UC_PROT_ALL);u.mem_write(base,image);u.mem_protect(base,image_size,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
 u.mem_map(THREAD,0x1000,unicorn.UC_PROT_ALL);u.mem_write(THREAD+0x30,struct.pack('<Q',THREAD));u.mem_write(THREAD+0x60,struct.pack('<Q',PROCESS));u.mem_protect(THREAD,0x1000,unicorn.UC_PROT_READ);u.reg_write(x86.UC_X86_REG_GS_BASE,THREAD)
 u.mem_map(PROCESS,0x1000,unicorn.UC_PROT_ALL);u.mem_write(PROCESS+0x10,struct.pack('<Q',main_base));u.mem_protect(PROCESS,0x1000,unicorn.UC_PROT_READ)
 u.mem_map(main_base,main_size,unicorn.UC_PROT_ALL);u.mem_write(main_base,main_image);u.mem_protect(main_base,main_size,unicorn.UC_PROT_READ)
 u.mem_map(STACK,STACK_SIZE,unicorn.UC_PROT_READ|unicorn.UC_PROT_WRITE)
 if seed:u.mem_write(STACK,rng.randbytes(STACK_SIZE))
 initial={name:(rng.getrandbits(64) if seed else 0) for name in REGS}
 initial.update({'rsp':MAIN_SP-8,'rbx':MAIN_SP,'rbp':MAIN_SP+0x80,'rcx':0,'rdx':0x25665cd7,'r8':0,'r9':0x60000100+seed*0x1000})
 for name,v in initial.items():u.reg_write(REGS[name],v)
 u.reg_write(x86.UC_X86_REG_EFLAGS,0x202)
 u.mem_write(MAIN_SP-8,struct.pack('<Q',0x2008d45c7));u.mem_write(MAIN_SP+0x748,struct.pack('<Q',initial['rbp']))
 result={'seed':seed,'initial_registers':{k:hex(v) for k,v in initial.items()},'steps':0,'image_reads':[],'thread_reads':[],'process_reads':[],'main_image_reads':[],'stack_writes':0,'stop':None,'last_va':None,'mnemonics':{},'trace_tail':[]}
 cache={};history=collections.deque(maxlen=20);mnemonics=collections.Counter();seen_reads=set()
 def code(uc,a,size,_):
  result['steps']+=1
  if not any(lo<=a<hi for lo,hi in allowed_ranges):
   end=FUNCTION_RANGES.get(a)
   if end is not None and a-base>=0x98c000 and end-a<=0x20000 and len(body_chain)<MAX_BODIES:
    allowed_ranges.append((a,end));body_chain.append({'start':hex(a),'end':hex(end),'at_step':result['steps']})
   else:result['stop']='helper exit before target instruction';result['exit_va']=hex(a);uc.emu_stop();return
  if result['steps']>MAX_INSNS:result['stop']='instruction budget';uc.emu_stop();return
  if a not in cache:
   ins=next(md.disasm(bytes(uc.mem_read(a,size)),a,count=1),None)
   if ins is None or ins.size!=size:result['stop']='decoder disagreement';uc.emu_stop();return
   cache[a]=(ins.mnemonic,ins.op_str)
  name,ops=cache[a]
  local_trampoline=False
  if name=='call':
   ins=next(md.disasm(bytes(uc.mem_read(a,size)),a,count=1))
   if ins.op_str.startswith('0x'):
    target=int(ins.op_str,16)
    if any(lo<=target<hi for lo,hi in allowed_ranges) and bytes(uc.mem_read(target,1))==b'\xc3':
     local_trampoline=True;local_calls.append({'call':a,'target':target,'return':a+size})
  elif name=='ret' and local_calls:
   stack_pointer=uc.reg_read(REGS['rsp']);return_va=struct.unpack('<Q',uc.mem_read(stack_pointer,8))[0]
   local_trampoline=local_calls[-1]['target']==a and local_calls[-1]['return']==return_va
   if local_trampoline:local_calls.pop()
  if name not in ALLOWED and not local_trampoline:result['stop']='blocked instruction '+name;uc.emu_stop();return
  result['last_va']=hex(a);mnemonics[name]+=1;history.append({'va':hex(a),'asm':name+' '+ops})
 def read(uc,access,a,size,value,_):
  if THREAD<=a<THREAD+0x1000:
   result['thread_reads'].append({'at':result['last_va'],'va':hex(a),'size':size})
   if a not in [THREAD+0x30,THREAD+0x60] or size!=8:
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
  if not STACK<=a or a+size>STACK+STACK_SIZE:result['stop']='blocked non-stack write';result['blocked_write']={'at':result['last_va'],'va':hex(a),'size':size,'value':hex(value&((1<<(size*8))-1))};uc.emu_stop();return
  result['stack_writes']+=1
 def invalid(uc,access,a,size,value,_):
  if result['stop'] is None:result['stop']='invalid or protected virtual-memory access'
  result['memory_fault']={'access':access,'va':hex(a),'size':size};return False
 u.hook_add(unicorn.UC_HOOK_CODE,code);u.hook_add(unicorn.UC_HOOK_MEM_READ,read);u.hook_add(unicorn.UC_HOOK_MEM_WRITE,write);u.hook_add(unicorn.UC_HOOK_MEM_INVALID,invalid)
 started=time.monotonic()
 try:u.emu_start(0x2008c15df,0,count=MAX_INSNS+1,timeout=5_000_000)
 except unicorn.UcError as e:
  result['emulator_error']=str(e)
  if result['stop'] is None:result['stop']='emulator error'
 if result['stop'] is None:result['stop']='emulator time/count bound'
 result['body_chain']=body_chain;result['admitted_ranges']=[[hex(lo),hex(hi)] for lo,hi in allowed_ranges];result['seconds']=round(time.monotonic()-started,6);result['registers']={name:hex(u.reg_read(reg)) for name,reg in REGS.items()};result['mnemonics']=dict(mnemonics);result['trace_tail']=list(history)
 frame=MAIN_SP-0x230
 result['saved_frame_after']={hex(off):hex(struct.unpack('<Q',u.mem_read(frame+off,8))[0]) for off in range(0,0x230,8)}
 return result

def main():
 results=[]
 for seed in range(6):
  result=one(seed)
  expected=result.get('blocked_write',{})
  if expected.get('va')!='0x200318e88' or expected.get('size')!=8 or expected.get('value')!=result['initial_registers']['r9'] or expected.get('at')!='0x2010621e1':raise RuntimeError('Expected context-initializer observation not reproduced: '+str(result['stop']))
  results.append(result);print(json.dumps({k:v for k,v in result.items() if k in ['seed','steps','stop','last_va','exit_va','memory_fault','emulator_error','seconds','body_chain','blocked_write']}),flush=True)
 report={'source_sha256':digest,'toolchain':{'unicorn':unicorn.__version__,'capstone':capstone.__version__,'pefile':pefile.__version__},'mode':'Offline Unicorn x86-64 emulation with synthetic registers/stack; copied image RX; no native execution or OS/device access','entry':'0x2008c15df','stack_padding':'zero for seed 0, deterministic random bytes for seeds 1-5','process_model':{'only_initialized_field':'+0x10 qword main image base; all other field reads stop','main_sha256':main_digest,'main_base':hex(main_base),'main_permissions':'read-only data, no executable permission'},'synthetic_thread_model':{'GS_BASE':hex(THREAD),'only_initialized_field':'+0x30 qword self pointer, +0x60 qword synthetic process pointer; every other field read stops'},'admitted_ranges':[[hex(a),hex(b)] for a,b in RANGES],'instruction_limit':MAX_INSNS,'per_run_timeout_us':5_000_000,'runs':results}
 (root/'derived/context-initializer-differential.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
