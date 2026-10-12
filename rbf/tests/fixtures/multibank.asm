; Multi-code-bank regression: BANK_SIZE=37, MEMORY_BANKS=1.
; Entry is bank 1; bank 0 must never execute. Output Y and RV0=0 mean success.
; The CALL at bank 1 offset 36 must save raw RA=37 and RAB=1.
; RET therefore executes bank 2 offset 0 twice: first with (PCB,PC)=(1,37),
; then with (2,0), matching the existing Rust VM's unnormalized return pair.
; Bank 2 also falls through normally into bank 3; JALR visits bank 4.

preamble:
    HALT
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

start:
    XORI S0, PCB, 1
    XORI T0, PC, 1
    OR S0, S0, T0
    LI A0, 0
    LI A1, 0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    CALL callee

return_path:
    ADDI A0, A0, 1
    XORI T0, A0, 2
    OR S0, S0, T0
    XORI T0, PCB, 2
    OR S0, S0, T0
    XORI T0, RA, 37
    OR S0, S0, T0
    XORI T0, RAB, 1
    OR S0, S0, T0
    XORI T0, A1, 1
    OR S0, S0, T0
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

fallthrough:
    XORI T0, PCB, 3
    OR S0, S0, T0
    LI T0, %bank(indirect_target)
    LI T1, %offset(indirect_target)
    JALR RA, T0, T1
    XORI T0, A1, 3
    OR S0, S0, T0
    XORI T0, RAB, 3
    OR S0, S0, T0
    XORI T0, RA, 5
    OR S0, S0, T0
    BNE S0, R0, L_failure
    LI T0, 89
    STORE T0, R0, R0
    LI RV0, 0
    HALT
L_failure:
    LI T0, 78
    STORE T0, R0, R0
    LI RV0, 1
    HALT
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP

callee:
    XORI T0, PCB, 4
    OR S0, S0, T0
    ADDI A1, A1, 1
    RET

indirect_target:
    XORI T0, PCB, 4
    OR S0, S0, T0
    ADDI A1, A1, 2
    RET

