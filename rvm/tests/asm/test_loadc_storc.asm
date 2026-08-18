; LOADC/STORC operate on instruction memory (PCB/PC addressing).
; Bank and address operands are registers only.

_start:
    LI T0, 0
    LI T1, payload
    LOADC R0, T0, T1

    ; payload is ADDI T7, T6, 0x42
    LI T2, 0x0A
    BNE X0, T2, fail
    LI T2, 22
    BNE X1, T2, fail
    LI T2, 21
    BNE X2, T2, fail
    LI T2, 0x42
    BNE X3, T2, fail

    ; Patch `slot` with LI A0, 65 and execute it
    LI X0, 0x0E
    LI X1, 7
    LI X2, 65
    LI X3, 0
    LI T1, slot
    STORC R0, T0, T1
slot:
    NOP
    STORE A0, R0, R0

    LI T2, 89
    STORE T2, R0, R0
    LI T2, 10
    STORE T2, R0, R0
    HALT

fail:
    LI T2, 78
    STORE T2, R0, R0
    LI T2, 10
    STORE T2, R0, R0
    HALT

payload:
    ADDI T7, T6, 0x42
