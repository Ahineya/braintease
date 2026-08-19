> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

Below is the full, up-to-date specification with a new § 11 Output-format examples that shows exactly how machine programs and data blobs are represented with the current macro set.


Ripple VM — Instruction-Set & System Specification

revision 2025-08-07-b

⸻

1. Fundamental data model

item	value	notes
Cell width	16-bit unsigned (0 … 65 535)
Instruction length	4 consecutive cells
Bank size	4096 instructions
Arithmetic	wrap-around modulo 65 536
Boolean	0 = false, 1 = true
R0	reads 0, writes are ignored


⸻

2. Register file

See 32-REGISTER-UPGRADE.md for the full register set.

⸻

3. Program address

absoluteCell = PCB·64 + PC·4 + localWord   // localWord ∈ {0,1,2,3}

All jumps manipulate PC only (unless you load PCB explicitly).

⸻

4. Instruction word layout

word0  word1  word2  word3
┌────┬────┬────┬────┐
│opc │ a  │ b  │ c  │
└────┴────┴────┴────┘

class	words 1-3 mean	used by
R	rd, rs, rt	ALU ops, JALR
I	rd, rs, imm	ALU-imm, memory, branches
I1	rd, imm, 0	LI
I2	rd, imm1, imm2	JAL
J	addr, 0, 0	in-bank absolute jump


⸻

5. Opcode map & behaviour

5.1 ALU (R)

hex	mnemonic	effect
00	NOP	—
01	ADD	rd ← rs + rt
02	SUB	rd ← rs − rt
03	AND	rd ← rs & rt
04	OR	rd ← rs | rt
05	XOR	rd ← rs ^ rt
06	SLL	rd ← rs << (rt & 15)
07	SRL	rd ← rs >> (rt & 15)
08	SLT	signed compare
09	SLTU	unsigned compare

5.2 ALU-immediate (I / I1)

hex	mnemonic	effect
0A	ADDI	rd ← rs + imm
0B	ANDI	rd ← rs & imm (zero-ext)
0C	ORI	rd ← rs | imm (zero-ext)
0D	XORI	rd ← rs ^ imm (zero-ext)
0E	LI	rd ← imm (no shift)
0F	SLLI	rd ← rs << imm
10	SRLI	rd ← rs >> imm

5.3 Memory (I)

hex	mnemonic	effect
11	LOAD rd, bank, addr 	rd ← MEM[bank * BANK_SIZE + addr]
12	STORE rd, bank, addr	MEM[bank * BANK_SIZE + addr] ← rd

LOAD and STORE address data memory. Bank and addr may be registers or immediates; RVM treats both operands as register indices.

5.3.1 Instruction-memory copies (R)

hex	mnemonic	effect
20	LOADC R0, bank, addr	X0..X3 ← IMEM[bank][addr] (4 instruction cells)
21	STORC R0, bank, addr	IMEM[bank][addr] ← X0..X3 (4 instruction cells)

LOADC and STORC address instruction memory using the same (bank, offset) scheme as PCB/PC. Both bank and addr are registers (no immediates). The first operand is unused and is conventionally R0. Destinations/sources are implicit: X0 (opcode cell), X1, X2, X3 (the three operand cells).

5.4 Control flow

hex	form	effect
13	JAL addr	RA ← PC+1, RAB ← PCB, PC ← addr, Do Not Increment PC flag set
14	JALR rd, 0, rs(assembler: JALR rd, rs)	rd ← PC+1, RAB ← PCB, PC ← rs, Do Not Increment PC flag set
15	BEQ rs, rt, imm	if equal → PC+=imm, Do Not Increment PC flag set
16	BNE rs, rt, imm	if not-equal, PC+=imm, Do Not Increment PC flag set
17	BLT rs, rt, imm	if signed less, PC+=imm, Do Not Increment PC flag set
18	BGE rs, rt, imm	if signed ≥, PC+=imm, Do Not Increment PC flag set

19 BRK debugger breakpoint

1A MUL rd, rs, rt	rd ← rs * rt
1B DIV rd, rs, rt	rd ← rs / rt
1C MOD rd, rs, rt	rd ← rs % rt

1D	MULI rd, rs, imm	rd ← rs * imm
1E	DIVI rd, rs, imm	rd ← rs / imm
1F	MODI rd, rs, imm	rd ← rs % imm

00	HALT	enter HALT state

All branch targets are bank-local; assembler emits a far jump as:

LI   PCB, bank(label)
JAL  addr(label)


⸻

6. Memory-mapped I/O

address	name	action
0x0000	OUT	write a byte → host stdout
0x0001	OUT_FLAG	host sets 1 when ready


⸻

7. Execution state machine

SETUP → RUNNING
while RUNNING:
fetch 4 words @ (PCB,PC)
execute
PC = PC + 1, if PC = 65536 then
  PCB = PCB + 1, PC = 0
UNLESS the instruction set a "Do Not Increment PC" flag
HALT ⇒ stop


⸻

8. Assembler rules
   •	Case-insensitive mnemonics.
   •	Immediates are unsigned 16-bit unless prefixed with -.
   •	JALR rd, rs ⇒ machine words opc=0x14, rd, 0, rs.
   •	Labels resolved per bank, far jumps auto-patched (see §5.4).

⸻



⸻



End of document