"""Exercise the actual offline emulator policy with synthetic instruction bytes."""
from pathlib import Path
import sys,json,struct
sys.path.insert(0,str(Path(__file__).resolve().parent/'analysis/toolchain/python'))
import emulate_runtime as model
from unicorn.x86_const import UC_X86_REG_RAX
entry=0x2008c15df
original_ctor=model.unicorn.Uc
engines=[]
def capture(*args,**kwargs):
 engine=original_ctor(*args,**kwargs);engines.append(engine);return engine
model.unicorn.Uc=capture
model.FUNCTION_RANGES={};model.MAX_INSNS=1000;model.image_size=0x900000
model.main_size=0x1000;model.main_image=b'\0'*0x1000

def run(code):
 engines.clear()
 image=bytearray(model.image_size);offset=entry-model.base;image[offset:offset+len(code)]=code;model.image=bytes(image)
 return model.one(0),engines[-1]
checks=[]
r,u=run(bytes.fromhex('48c7c02a0000000f05'))
assert r['stop']=='blocked instruction syscall' and u.reg_read(UC_X86_REG_RAX)==42,r
checks.append('Arithmetic executes; SYSCALL blocked before execution')
r,u=run(bytes.fromhex('e85b000000'))
assert r['stop']=='blocked instruction call' and int(r['registers']['rsp'],16)==model.MAIN_SP-8,r
checks.append('Unapproved CALL blocked before stack mutation')
r,u=run(bytes.fromhex('c3'))
assert r['stop']=='blocked instruction ret',r
checks.append('Unapproved RET blocked')
r,u=run(bytes.fromhex('e80d00000048c7c02a000000eb0590909090c3'))
assert r['stop']=='helper exit before target instruction' and u.reg_read(UC_X86_REG_RAX)==42 and int(r['registers']['rsp'],16)==model.MAIN_SP-8 and r['stack_writes']==1,r
checks.append('Checked local CALL/RET trampoline preserves stack balance')
address=model.base+0x1000
r,u=run(b'\x48\xb8'+struct.pack('<Q',address)+bytes.fromhex('48c70034120000'))
assert u.mem_read(address,8)==b'\0'*8 and ('blocked_write' in r or r.get('memory_fault',{}).get('va')==hex(address)),r
checks.append('Copied image is write-protected and remains unchanged')
address=0x12345000
r,u=run(b'\x48\xb8'+struct.pack('<Q',address)+bytes.fromhex('488b00'))
assert r.get('memory_fault',{}).get('va')==hex(address),r
checks.append('Unmapped access blocked without automatic mapping')
r,u=run(bytes.fromhex('ebfe'))
assert r['steps']<=model.MAX_INSNS+1 and r['stop'] in ['instruction budget','emulator time/count bound'],r
checks.append('Infinite loop bounded by instruction count')
r,u=run(bytes.fromhex('65488b0425300000000f05'))
assert r['stop']=='blocked instruction syscall' and u.reg_read(UC_X86_REG_RAX)==model.THREAD,r
checks.append('Synthetic thread self-pointer is the modeled value')
r,u=run(bytes.fromhex('65488b0425380000000f05'))
assert r['stop']=='unmodeled synthetic thread field',r
checks.append('Unknown thread fields stop instead of silently supplying zero')
r,u=run(b'\x48\xb8'+struct.pack('<Q',model.PROCESS+0x18)+bytes.fromhex('488b00'))
assert r['stop']=='unmodeled synthetic process field',r
checks.append('Unknown process fields stop instead of silently supplying zero')
r,u=run(b'\x48\xb8'+struct.pack('<Q',model.main_base)+bytes.fromhex('ffe0'))
assert 'FETCH_PROT' in r.get('emulator_error',''),r
checks.append('Main EXE copy has no executable permission')
model.unicorn.Uc=original_ctor
out={'fixture_source':'Synthetic x86-64 byte sequences, not Sony code','checks':checks,'passed':len(checks)}
(model.root/'derived/emulator-policy-controls.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
