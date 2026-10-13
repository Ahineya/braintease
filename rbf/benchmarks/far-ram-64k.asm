; BANK_SIZE=64000 MEMORY_BANKS=256
; Check far reads/writes, a RAM-bank boundary and 16-bit value wraparound.
start:
    LI S0, 64
    LI T0, 136
    LI T1, 63968
    LI T2, 65520
    LI T3, 64000
L_loop:
    STORE T2, T0, T1
    LOAD A0, T0, T1
    BNE A0, T2, L_fail
    ADDI T2, T2, 1
    ADDI T1, T1, 1
    BNE T1, T3, L_next
    LI T1, 0
    ADDI T0, T0, 1
L_next:
    ADDI S0, S0, -1
    BNE S0, R0, L_loop
    LI T0, 255
    LI T1, 63999
    STORE T2, T0, T1
    LOAD A0, T0, T1
    BNE A0, T2, L_fail
    LI A0, 89
    STORE A0, R0, R0
    LI RV0, 0
    HALT
L_fail:
    LI A0, 78
    STORE A0, R0, R0
    LI RV0, 1
    HALT
