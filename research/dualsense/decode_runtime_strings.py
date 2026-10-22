"""Recover only verified UTF-16 XOR/ROL5 feedback loops; never execute input code."""
from pathlib import Path
import sys,re,json,hashlib,bisect,struct
sys.path.insert(0,str(Path(__file__).resolve().parent/'analysis/toolchain/python'))
import pefile,capstone
root=Path(__file__).resolve().parent/'analysis/remoteplay-win'
raw=(root/'payload/Data1/runtime.dll').read_bytes();digest=hashlib.sha256(raw).hexdigest()
if digest!='ae4ae997261fd9da4dddbba0edea6244bcefe2f24899221f793826b4da497e87':raise RuntimeError('Unexpected runtime input identity')
p=pefile.PE(data=raw);base=p.OPTIONAL_HEADER.ImageBase;md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True

def r64(n):
 return {'eax':'rax','ebx':'rbx','ecx':'rcx','edx':'rdx','esi':'rsi','edi':'rdi','ebp':'rbp','esp':'rsp','ax':'rax','bx':'rbx','cx':'rcx','dx':'rdx'}.get(n,n[:-1] if n.startswith('r') and n[-1:] in ['d','w','b'] else n)
def reg(i,o):return r64(i.reg_name(o.reg)) if o.type==capstone.x86.X86_OP_REG else None
def decode_at(a,n=100):
 o=p.get_offset_from_rva(a-base);return list(md.disasm(raw[o:o+n],a))

def decode_words(encrypted,seed):
 if len(encrypted)%2:raise ValueError('UTF-16 ciphertext must have even length')
 plain=bytearray();v=seed&((1<<64)-1)
 for (word,) in struct.iter_unpack('<H',encrypted):
  plain.extend(struct.pack('<H',(v&65535)^word));v=(((v<<5)|(v>>59))^word)&((1<<64)-1)
 return bytes(plain)

def recognize(a):
 seq=decode_at(a,100)
 if len(seq)<10:return None
 seq=seq[:10];load,copy,xor,store,rol,zext,feedback,add,cmp,jne=seq
 if [i.mnemonic for i in seq]!=['movzx','mov','xor','mov','rol','movzx','xor','add','cmp','jne']:return None
 if len(load.operands)!=2 or load.operands[1].type!=capstone.x86.X86_OP_MEM or load.operands[1].size!=2:return None
 mem=load.operands[1].mem
 if mem.scale!=1 or mem.disp or not mem.base or not mem.index:return None
 cipher=reg(load,load.operands[0]);state=reg(rol,rol.operands[0]);temp=reg(copy,copy.operands[0])
 if not state or rol.operands[0].size!=8 or rol.operands[1].type!=capstone.x86.X86_OP_IMM or rol.operands[1].imm!=5:return None
 if reg(copy,copy.operands[1])!=state or reg(xor,xor.operands[0])!=temp or reg(xor,xor.operands[1])!=cipher:return None
 if store.operands[0].type!=capstone.x86.X86_OP_MEM or store.operands[0].size!=2 or reg(store,store.operands[1])!=temp:return None
 if reg(zext,zext.operands[0])!=cipher or reg(zext,zext.operands[1])!=cipher or zext.operands[1].size!=2:return None
 if reg(feedback,feedback.operands[0])!=state or reg(feedback,feedback.operands[1])!=cipher:return None
 counter=reg(add,add.operands[0])
 if add.operands[1].type!=capstone.x86.X86_OP_IMM or add.operands[1].imm!=2:return None
 if reg(cmp,cmp.operands[0])!=counter or cmp.operands[1].type!=capstone.x86.X86_OP_IMM:return None
 length=cmp.operands[1].imm
 if length<=0 or length>4096 or length%2:return None
 if jne.operands[0].type!=capstone.x86.X86_OP_IMM or jne.operands[0].imm!=a:return None
 names={r64(load.reg_name(mem.base)),r64(load.reg_name(mem.index))}
 if len(names)!=2 or counter not in names:return None
 source=(names-{counter}).pop()
 if len({source,counter,state,cipher,temp})!=5:return None
 destmem=store.operands[0].mem
 if destmem.base!=capstone.x86.X86_REG_RSP or r64(store.reg_name(destmem.index))!=counter or destmem.scale!=1:return None
 # A bounded straight-line setup must provide all three values. Only the
 # expected register writes and NOPs are accepted, so no hidden clobber is ignored.
 for begin in range(max(base,a-64),a):
  offset=p.get_offset_from_rva(begin-base)
  if not (raw[offset] in range(0x48,0x50) and raw[offset+1] in range(0xb8,0xc0)):continue
  setup=decode_at(begin,a-begin)
  if not setup or setup[-1].address+setup[-1].size!=a:continue
  seed=None;source_va=None;zero=False;valid=True
  for i in setup:
   ops=i.operands
   if i.mnemonic=='nop':continue
   if i.mnemonic=='movabs' and reg(i,ops[0])==state and ops[0].size==8 and ops[1].type==capstone.x86.X86_OP_IMM:seed=ops[1].imm&((1<<64)-1)
   elif i.mnemonic=='xor' and reg(i,ops[0])==counter and reg(i,ops[1])==counter and ops[0].size==4:zero=True
   elif i.mnemonic=='lea' and reg(i,ops[0])==source and ops[1].mem.base==capstone.x86.X86_REG_RIP and not ops[1].mem.index:source_va=i.address+i.size+ops[1].mem.disp
   else:valid=False;break
  if valid and seed is not None and source_va is not None and zero:
   o=p.get_offset_from_rva(source_va-base);encrypted=raw[o:o+length]
   if len(encrypted)!=length:raise RuntimeError('Truncated encrypted string')
   plain=decode_words(encrypted,seed)
   try:text=plain.decode('utf-16le')
   except UnicodeDecodeError:text=None
   return {'setup_va':hex(begin),'loop_va':hex(a),'source_va':hex(source_va),'source_file_offset':hex(o),'bytes':length,'seed':hex(seed),'text':text,'decoded_hex':plain.hex(),'verified_instructions':[{'va':hex(i.address),'bytes':i.bytes.hex(),'asm':i.mnemonic+' '+i.op_str} for i in setup+seq]}
 return None

def main():
 out=[];attempts=0
 for section in p.sections:
  data=section.get_data();start=base+section.VirtualAddress
  for match in re.finditer(rb'[\x48-\x4f]\xc1[\xc0-\xc7]\x05',data):
   rotate=start+match.start()
   for delta in range(10,33):
    attempts+=1
    result=recognize(rotate-delta)
    if result and result['loop_va'] not in {x['loop_va'] for x in out}:out.append(result)
 report={'source_sha256':digest,'method':'Exact ten-instruction UTF-16 XOR/ROL5 ciphertext-feedback loop plus bounded validated seed/zero-counter/RIP-source setup','candidate_alignments':attempts,'verified_loops':len(out),'strings':out}
 (root/'runtime-native-strings.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Candidate alignments',attempts,'verified loops',len(out))
 for x in out:print(x['loop_va'],repr(x['text']))

if __name__=='__main__':main()
