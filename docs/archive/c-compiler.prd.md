> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

Ripple C99 Compiler — Product Requirements Document (PRD)

1) Goal & Scope

Build rcc: a C99 (freestanding) compiler that targets Ripple VM ISA and emits assembly consumable by rasm → rlink → macro BF. Focus on small, predictable codegen over aggressive optimization. First-class support for IDE/debugger.

In-scope (MVP)
•	C99 freestanding subset:
•	Types: void, _Bool, char, signed/unsigned char, short, unsigned short, int, unsigned int, long, unsigned long, pointer types
•	Qualifiers: const, volatile (codegen-aware but minimal)
•	Control: if/else, switch, while, do/while, for, break/continue, return
•	Expressions: all integer ops, logical ops, comparisons, assignment, address-of/deref, arrays, struct/union (no bitfields in MVP), function calls (no varargs in MVP)
•	Initializers for scalars and aggregates
•	Separate compilation: .c → .asm → .pobj → link
•	Debug info (symbols + line maps) for IDE’s disassembly view
•	Basic optimizations: constant folding, copy-prop, dead code elimination, peephole, tail-merge

Out-of-scope (MVP / later)
•	Floating point; long long; varargs; bitfields; setjmp/longjmp; threads; malloc implementation (we ship stubs); full hosted libc.

⸻

2) Target Machine Model (Ripple)

Word & Cells
•	Cell/word size: 16-bit unsigned storage (tape cells).
•	Byte model: char is 8-bit value, but stored in one 16-bit cell (low 8 bits used).
•	Endianness: N/A for single-cell scalars; for multi-cell integers, little-endian in memory (low word first).

Addressing / Pointers
•	ISA uses (bank, addr) pairs. For MVP the compiler uses flat pointers: a pointer value is held in one 16-bit register and mapped onto the addr operand. (Current string/array usage: LOAD r, R0, ptr.)
•	Stretch goal (V2): full 32-bit pointers (bank, addr) in two registers for >64K data.

Registers & Special
•	R0 reserved as constant zero (compiler never writes it).
•	PC/PCB implicit; RA/RAB are link registers modified by JAL/JALR.
•	Proposed ABI names:
•	R3 — return value, arg0
•	R4..R8 — arg1..arg5 (also caller-saved temps)
•	R9..R12 — callee-saved
•	R13 — SB (Stack Bank)
•	R14 — SP (Stack Pointer, “addr” field)
•	R15 — FP (Frame Pointer / base pointer)
•	RA/RAB — link registers (clobbered by call)
•	Caller-saved: R3..R8, R11 (incl. return)
•	Callee-saved: R9, R10, R12, FP(R15), SP(R14), SB(R13)

Stack & Frames
•	Location: all stack accesses use (bank=SB=R13, addr=SP/FP).
•	Growth: upwards (SP += size). (Matches PUSH/POP expansion habit.)
•	Frame layout (low → high):

[saved RA] [saved RAB] [saved FP] [saved callee-saved regs used]
[locals ...]
[outgoing arg spill area (optional)]


	•	Prologue (sketch):

STORE FP, SB, SP
ADDI  SP, SP, 1
STORE RA, SB, SP         ; if function will call others
ADDI  SP, SP, 1
ADD   FP, SP, R0
ADDI  SP, SP, <locals>


	•	Epilogue:

ADD   SP, FP, R0
SUBI  SP, SP, 1          ; to RA slot
LOAD  RA, SB, SP
SUBI  SP, SP, 1          ; to saved FP slot
LOAD  FP, SB, SP
JALR  R0, R0, RA



⸻

3) Data Layout & C Type Mapping

C type	Size (cells)	Alignment	Notes
_Bool	1	1	0 or 1
char / signed char	1	1	low 8 bits used
unsigned char	1	1
short	1	1	16-bit
unsigned short	1	1	16-bit
int	1	1	16-bit (ILP16)
unsigned int	1	1	16-bit
long	2	1	32-bit (two cells, little-endian)
unsigned long	2	1	32-bit
pointer	1 (MVP)	1	flat pointer (bank only), addr=R0 at use
struct/union	sum of fields	1	padded to cell boundaries; no bitfields
enum	1	1	16-bit signed

V2: enable 32-bit pointers (2 cells), 64-bit long long (4 cells).

⸻

4) Instruction Selection (Lowering)

Use only ISA ops from the current Ripple VM version assembly reference:
•	Int add/sub/logic: ADD/SUB/AND/OR/XOR, ADDI/ANDI/ORI/XORI
•	Shifts: SL/SR and immediates SLI/SRI
•	Comparisons: signed SLT, unsigned SLTU or branch forms BLT/BGE; equality via BEQ/BNE
•	Loads/stores: LOAD rd, bankReg, addrReg and STORE rs, bankReg, addrReg
•	In MVP, bankReg = R0; addrReg holds the flat address/pointer.
•	Calls:
•	Direct: JAL R0, bankImm, addrImm (assembler/linker resolve target)
•	Indirect: JALR R0, bankReg, addrReg (used for function pointers)
•	RA receives PC+1 by ISA; compiler saves RA if making nested calls.
•	Return: JALR R0, R0, RA
•	I/O (putchar): STORE r, R0, R0 writes byte to device.

Software sequences (libcalls or builtins) when needed:
•	32-bit arithmetic (long) → helper routines
•	memcpy/memset/memmove, strcmp, etc.

⸻

5) Calling Convention (C ABI)
   •	Parameter passing:
   •	Arg0 → R3, Arg1 → R4, … up to R8.
   •	Overflow args spilled to stack at call site (highest to lowest), caller computes addresses and stores via SB,SP.
   •	Return values:
   •	16-bit integer/pointer → R3
   •	32-bit long → R4:R3 (low in R3)
   •	structs ≤ 2 cells returned in regs like integers; larger via sret: hidden pointer in R3 to caller-allocated buffer; function writes and returns nothing (R3 undefined).
   •	Caller responsibilities:
   •	Preserve callee-saved (R9,R10,R12,R13(SB),R14(SP),R15(FP)) if needed.
   •	Assume RA/RAB, R3..R8, R11 clobbered.
   •	Callee responsibilities:
   •	Save/restore any used callee-saved regs.
   •	Save RA if making calls; otherwise tail-call allowed: JALR R0, bankReg, addrReg without restoring RA.

⸻

6) Runtime & Start-up
   •	crt0 (minimal):
   •	Initialize SB=DATA_BANK (config), SP=stack_base, FP=stack_base-1.
   •	Zero .bss (option).
   •	Call main(int argc,char**argv) as main() (MVP no args).
   •	On return, call _exit(status) or HALT.
   •	libc (freestanding subset):
   •	void putchar(int), void puts(const char*), void* memcpy(void*,const void*,size_t), void* memset(void*,int,size_t), int strcmp(const char*,const char*), …
   •	I/O mapping:
   •	putchar(c) → STORE R3, R0, R0 (low 8 bits used)

⸻

7) Compiler Architecture
   •	Front end: C99 parser + semantic analysis
   •	IR: simple 3-address SSA-esque (or linear TAC) supporting:
   •	integer ops, branches, calls, load/store, phi (if SSA)
   •	Middle end (MVP): const fold, DCE, copy-prop, local CSE, strength-reduce x<<k/x>>k, branch folding.
   •	Backend:
   •	Instruction selection by patterns from IR → Ripple ops.
   •	Register allocation: linear scan over R3..R12 with spill to stack.
   •	Prologue/epilogue & call lowering per ABI.
   •	Peephole pass: remove ADD rd, rs, R0; coalesce LI+use; fuse compare+branch to BEQ/BNE/BLT/BGE.
   •	Emission: textual Ripple assembly with sections, labels, and canonical syntax. Pipe to rasm/rlink.

⸻

8) Tooling & CLI

Binary: rcc

rcc [files...] [-c|-S] [-o out] [-I dir] [-O0|-O1|-O2] [-g]
[-mflat-ptr| -mptr32 ] [-mrtlib=path] [--emit-prologue]
[--stack-bank=N] [--stack-base=ADDR]

	•	-S → emit .asm
	•	-c → emit .pobj via calling rasm
	•	Default pipeline: .c -> .asm -> rasm -> .pobj
	•	-g → line tables & symbols (labels like __Lfile_line), register maps at call sites.
	•	--driver convenience: rcc main.c -o app.bf runs full chain (rasm, rlink, bfm).

Output layout:
•	.text (code) in program banks, .rodata/.data/.bss in data bank (SB).
•	Linker script picks concrete bank indices.

⸻

9) Codegen Examples

Example 1: int add(int a,int b){ return a+b; }

; a in R3, b in R4, ret in R3
add:
ADD   R3, R3, R4
JALR  R0, R0, RA

Example 2: caller saving and call

int sq(int x){ return x*x; }
int f(int a,int b){ return sq(a) + sq(b); }

sq:
; prologue omitted (leaf)
MUL R3, R3, R3  ; R3 = x*x
ADD   R3, R3, R0          ; result in R3
JALR  R0, R0, RA

f:
; save RA because we'll call
STORE RA, R13, R14        ; push RA
ADDI  R14, R14, 1

    ADD   R4, R3, R0          ; a -> R4 (since arg0 is R3 for call)
    ADD   R3, R4, R0
    JAL   bank(sq), addr(sq)  ; R3 = sq(a)

    ADD   R5, R3, R0          ; save sq(a) in caller-saved R5

    ADD   R3, R0, R0          ; prepare arg0=b now
    ADD   R3, R4, R0          ; (load b into R3 if needed)
    ; actually b was originally in R4; ensure correct move here
    JAL   bank(sq), addr(sq)  ; R3 = sq(b)

    ADD   R3, R3, R5          ; add partials

    SUBI  R14, R14, 1         ; pop RA
    LOAD  RA, R13, R14
    JALR  R0, R0, RA

Example 3: pointer load/store (flat pointer)

void putc(char c){ *(volatile unsigned char*)0 = c; }

putc:
STORE R3, R0, R0      ; device (0,0)
JALR  R0, R0, RA


⸻

10) Optimizations Roadmap
    •	O0: straight lowering, minimal peephole.
    •	O1: common subexpr elim (local), copy-prop, branch folding, tail calls.
    •	O2: loop invariant code motion, basic register coalescing; strength reduction; inline small leafs; combine SLT+BEQ → BLT/BGE.

⸻

11) Testing & Validation
    •	Unit: per-pass tests (parser, type checker, regalloc).
    •	Integration: compile known samples: hello, FizzBuzz, Fibonacci (iterative & recursive), small libc tests.
    •	ISA conformance: differential tests vs hand-written assembly.
    •	Debug: stepping confirms RA/PC changes; verify stack traces in IDE.
    •	Perf: cycle/step counts on interpreter; size of .bfm.

⸻

12) Deliverables & Milestones
    1.	M1 – Backend skeleton (2–3 wks)
          ISA emitter, ABI, prologue/epilogue, calls, loads/stores, arithmetic, branches. Hello world works (uses STORE R0,R0).
    2.	M2 – Front end & IR (3–4 wks)
          Parse C subset, type checking, IR, lowering. Run toy programs (no structs).
    3.	M3 – Data, structs, arrays (2 wks)
          Aggregates, address-of/deref, global data emission, .rodata strings.
    4.	M4 – Runtime + libc mini (2 wks)
          crt0, math helpers, memcpy/memset, puts/putchar.
    5.	M5 – Optimizations + Debug (2 wks)
          O1, line maps, symbol dumping for IDE, verify stepping.
    6.	M6 – Toolchain integration (1 wk)
          rcc driver orchestrating rasm/rlink, docs, examples.

⸻

13) Risks & Mitigations
    •	Stack bank overflow: configurable --stack-bank and guard helpers in crt0.

⸻

14) Documentation & Examples
    •	Ship ABI.md (registers, frames, call rules), rcc.md (CLI), and samples/
    •	hello.c, fizzbuzz.c, fib.c, structs.c, pointers.c.
