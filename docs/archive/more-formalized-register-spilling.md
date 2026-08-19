> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

Gotcha—here’s a step-by-step, spill-only register assignment algorithm (no virtual regs) tailored to your Ripple ISA.

Assumptions & Conventions
•	Hardware regs:
•	R0 = 0
•	RA/RAB used by JAL/JALR (untouchable by allocator)
•	Chosen calling convention (simple):
•	SP = R14, FP = R15, SB = R13 (stack bank id), GB = R12 (globals/rodata bank)
•	Return value in R3
•	Caller-saved: all allocatable regs
•	Allocatable pool: POOL = [R5, R6, R7, R8, R9, R10, R11]
•	Word size: 1 cell (your ISA semantics); array index scale folded into offset calc
•	Memory ops: LOAD rd, bankReg, addrReg / STORE rs, bankReg, addrReg

Stack Frame Layout (per function)

FP+0   .. FP+L-1     : locals
FP+L   .. FP+L+S-1   : spill slots (temps)

Sizes L and S are known after local/temporary planning.

Prologue

ADD   FP, SP, R0
ADDI  SP, SP, -(L+S)

Epilogue

ADD   SP, FP, R0
JALR  R0, R0, RA

Data Structures

Free = stack/list of regs from POOL
MapRegToSlot[reg] = spill-slot offset or ⊥
MapValToSlot[id]  = spill-slot offset or ⊥   ; optional if you want to reload specific temps
LRU = queue of “in-use” regs (most-recently-used at tail)

Helper Routines

AddrLocal(offset) → r

r ← getReg()
ADD   r, FP, R0
ADDI  r, r, offset
return r

AddrGlobal(offset) → r

r ← getReg()
LI    r, offset
; if absolute addressing not desired, do ADD to a GP base instead
return r

spill(reg)

slot ← MapRegToSlot[reg]
if slot = ⊥ then
slot ← fresh_spill_slot()
MapRegToSlot[reg] ← slot
tmp ← getScratchAddr(slot)       ; uses R12 as scratch
STORE reg, SB, tmp
mark reg free (but don’t push to Free yet; caller will overwrite)

reload(slot) → reg

reg ← getReg()
tmp ← getScratchAddr(slot)
LOAD reg, SB, tmp
return reg

getScratchAddr(slot) → rTmp

ADD  R12, FP, R0
ADDI R12, R12, (L + slot)
return R12

getReg() → reg

if Free not empty: reg ← pop(Free); push reg into LRU; return reg
victim ← pickVictim(LRU)         ; LRU front
spill(victim)
push victim back to Free
reg ← pop(Free); push reg into LRU; return reg

freeReg(reg)

remove reg from LRU
MapRegToSlot[reg] stays as-is (for potential reload)
push reg to Free

pickVictim(LRU)
Return the least-recently-used reg from LRU (front). (FIFO works too.)

Expression Codegen (Sethi–Ullman + greedy)

Define need(n):

need(Const/LoadLocal/LoadGlobal/LoadPtr) = 1
need(Unary u)  = need(child)
need(Binary b) =
if need(L)=need(R) then need(L)+1 else max(need(L), need(R))

EmitExp(n) → reg

switch kind(n):
case Const(k):
r ← getReg()
LI r, k
return r

case LoadLocal(off):
addr ← AddrLocal(off)
r ← getReg()
LOAD r, SB, addr
freeReg(addr)
return r

case LoadGlobal(off):
addr ← AddrGlobal(off)
r ← getReg()
LOAD r, GB, addr
freeReg(addr)
return r

case LoadPtr(ptrExp, byteOff):
rp ← EmitExp(ptrExp)
if byteOff ≠ 0: ADDI rp, rp, byteOff
r ← getReg()
LOAD r, SB_or_GB_from_provenance(ptrExp), rp
freeReg(rp)
return r

case Unary(op, x):
rx ← EmitExp(x)
; apply op using available ISA (e.g., NOT via XORI, NEG via SUB)
return rx                   ; in-place

case Binary(op, a, b):
; order children by need(): evaluate larger first
if need(a) < need(b) then swap(a,b)

    ra ← EmitExp(a)
    rb ← EmitExp(b)

    switch op:
      case '+': ADD  ra, ra, rb
      case '-': SUB  ra, ra, rb
      case '&': AND  ra, ra, rb
      case '|': OR   ra, ra, rb
      case '^': XOR  ra, ra, rb
      case '<<': SL  ra, ra, rb
      case '>>': SR  ra, ra, rb
      case '<':  SLT ra, ra, rb
      case 'u<': SLTU ra, ra, rb
      ; if op not in ISA (e.g., MUL): call helper or emit loop

    freeReg(rb)
    return ra

Statement Codegen

StoreLocal(off, exp)

rv   ← EmitExp(exp)
addr ← AddrLocal(off)
STORE rv, SB, addr
freeReg(rv); freeReg(addr)

StoreGlobal(off, exp) — same but bank = GB

StorePtr(ptrExp, byteOff, valExp)

rp ← EmitExp(ptrExp)
if byteOff ≠ 0: ADDI rp, rp, byteOff
rv ← EmitExp(valExp)
STORE rv, SB_or_GB_from_provenance(ptrExp), rp
freeReg(rv); freeReg(rp)

If / While conditions

rc ← EmitExp(cond)
BEQ rc, R0, label_false_or_exit
freeReg(rc)
; then/loop body...

At statement boundaries: all temporaries must be either stored (if needed) or freed. No temps cross basic blocks.

Calls

Before JAL

; Spill everything conservatively (simple & correct)
for each reg in LRU: spill(reg); freeReg(reg)
; Evaluate args and place per ABI (e.g., on stack or in R5..R8)
JAL bankImm, addrImm
; Result expected in R3 (by convention)

(When you track “live” flags later, you can avoid spilling dead regs.)

Branching & Joins
•	Because temps never cross statements/blocks, no merge mapping is required.
•	Values that must survive are in memory (locals/globals) and are reloaded when needed.

Function Return
•	Place return value in R3 and R4 before epilogue.
•	Emit epilogue (restore SP, JALR R0,R0,RA).

Bank Selection for Pointers
•	At compile time, each pointer expression carries its provenance: SB (stack) or GB (globals/rodata).
•	LOAD/STORE pick SB or GB accordingly. (If mixed is possible, carry a (bank,value) pair in codegen, or normalize pointers into a known region.)

Correctness Guarantees
•	When out of regs, you spill to the frame—always correct.
•	Before calls, all temps are spilled—no clobber.
•	Across branches, only memory persists—no reg mismatch at joins.

⸻
