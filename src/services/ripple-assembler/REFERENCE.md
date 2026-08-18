; ==========================================================
; Ripple VM — Quick Reference (effects + calling details)
; ==========================================================
; General:
; - 16-bit registers, arithmetic wraps (mod 65536).
; - R0 is used as zero in code; avoid writing to it.
; - PC auto-increments after each instruction UNLESS a taken
;   branch/JAL/JALR sets a "no-inc" flag first.
; - Banks: PCB selects bank; addresses are 16-bit.
; - MMIO: STORE x, R0, R0  -> emits byte x to OUT (console).
; ==========================================================


; ----------------------------------------------------------
; Arithmetic / Logic (register–register)
; ----------------------------------------------------------
ADD  rd, rs, rt     ; rd = (rs + rt) & 0xFFFF
SUB  rd, rs, rt     ; rd = (rs - rt) & 0xFFFF
AND  rd, rs, rt     ; rd = rs & rt
OR   rd, rs, rt     ; rd = rs | rt
XOR  rd, rs, rt     ; rd = rs ^ rt
SL   rd, rs, rt     ; rd = (rs << (rt & 0xF)) & 0xFFFF   ; logical
SR   rd, rs, rt     ; rd = (rs >> (rt & 0xF))            ; logical, zero-fill
SLT  rd, rs, rt     ; rd = ((int16)rs < (int16)rt) ? 1 : 0
SLTU rd, rs, rt     ; rd = (rs < rt) ? 1 : 0             ; unsigned
MUL rd, rs, rt     ; rd = (rs * rt) & 0xFFFF            ; multiply (mod 65536)
DIV rd, rs, rt     ; rd = (rs / rt) & 0xFFFF            ; integer division (mod 65536)
MOD rd, rs, rt     ; rd = (rs % rt) & 0xFFFF            ; remainder (mod 65536)


; ----------------------------------------------------------
; Arithmetic / Logic (immediate)
; ----------------------------------------------------------
LI   rd, imm        ; rd = imm
ADDI rd, rs, imm    ; rd = (rs + imm) & 0xFFFF
ANDI rd, rs, imm    ; rd = rs & imm
ORI  rd, rs, imm    ; rd = rs | imm
XORI rd, rs, imm    ; rd = rs ^ imm
SLI  rd, rs, imm    ; rd = (rs << (imm & 0xF)) & 0xFFFF
SRI  rd, rs, imm    ; rd = (rs >> (imm & 0xF))           ; logical
MULI rd, rs, imm    ; rd = (rs * imm) & 0xFFFF           ; multiply (mod 65536)
DIVI rd, rs, imm    ; rd = (rs / imm) & 0xFFFF           ; integer division (mod 65536)
MODI rd, rs, imm    ; rd = (rs % imm) & 0xFFFF           ; remainder (mod 65536)


; ----------------------------------------------------------
; Memory  (bank & addr are *registers*; use R0 for zero)
; ----------------------------------------------------------
LOAD  rd, bank, addr    ; rd = MEM[bank][addr]          ; data memory
STORE rs, bank, addr    ; MEM[bank][addr] = rs          ; data memory
LOADC R0, bank, addr    ; X0..X3 = IMEM[bank][addr]     ; 4 instruction cells
STORC R0, bank, addr    ; IMEM[bank][addr] = X0..X3     ; 4 instruction cells
; SPECIAL: STORE x, R0, R0 -> print byte x

; Common patterns:
;   LOAD  rch, R0, rptr  ; rch = *(bank0 + rptr)
;   STORE rch, R0, R0    ; putchar(rch)
;   ADDI  rptr, rptr, 1  ; advance pointer


; ----------------------------------------------------------
; Branches (PC-relative immediate; labels resolved by assembler)
; ----------------------------------------------------------
BEQ rs, rt, target   ; if (rs == rt) PC <- target, no auto-inc this cycle
BNE rs, rt, target   ; if (rs != rt) PC <- target, no auto-inc
BLT rs, rt, target   ; if ((int16)rs <  (int16)rt) jump (signed)
BGE rs, rt, target   ; if ((int16)rs >= (int16)rt) jump (signed)

; Effect details:
; - On a TAKEN branch, microcode computes the new PC from the
;   branch site + offset (assembler handles labels) and sets
;   "no-inc". On NOT taken, normal PC+1 happens.


; ----------------------------------------------------------
; Calls / Jumps
; ----------------------------------------------------------
JAL  bankImm, addrImm    ; Call absolute (immediates)
; Effects (under the hood):
;   RA  <- PC + 1         ; return address (within caller bank)
;   RAB <- PCB            ; return bank
;   PCB <- bankImm
;   PC  <- addrImm
;   (no auto-inc this cycle)

JALR bankReg, addrReg    ; Call absolute (registers)
; Effects:
;   RA  <- PC + 1
;   RAB <- PCB
;   PCB <- bankReg
;   PC  <- addrReg
;   (no auto-inc this cycle)

HALT                     ; Stop the machine
BRK                      ; Breakpoint (spins forever)

; Return idioms:
;   ; same-bank return (e.g., all code in bank 0)
;   JALR R0, R0, RA
;
;   ; cross-bank-safe return (restores caller bank from RAB)
;   JALR R0, RAB, RA

; Callee prologue/epilogue pattern (save/restore RA if clobbering):
;   ADD  R9, RA, R0     ; save RA (scratch)
;   ... body ...
;   JALR R0, R0, R9     ; return (or JALR R0, RAB, R9 for cross-bank)


; ----------------------------------------------------------
; Tiny, useful snippets
; ----------------------------------------------------------

; Print NUL-terminated string at R8 (bank 0)
print_string:
ADD  R9, RA, R0
ps_loop:
LOAD  R10, R0, R8
BEQ   R10, R0, ps_done
STORE R10, R0, R0
ADDI  R8,  R8, 1
JAL   R0,  R0, ps_loop
ps_done:
JALR  R0,  R0, R9

; Compare / branch example (signed)
cmp_demo:
SLT  R11, R3, R4      ; R11=1 if R3<R4 (signed)
BNE  R11, R0, less
; ... R3 >= R4 path ...
JAL  R0, R0, after
less:
; ... R3 < R4 path ...
after:

; Call/return across banks safely
caller:
; call func at (bank=2, addr=label 'func')
JAL  2, func
; ... resumes here ...
HALT

func:
ADD  R9, RA, R0       ; save RA if needed
; ... work ...
JALR R0, RAB, R9      ; return to caller bank/addr
