; BANK_SIZE=512 MEMORY_BANKS=16
; Repeatedly access the final word in the last RAM bank.
start:
    LI S0, 16
    LI T0, 15
    LI T1, 511
    LI T2, 13
L_loop:
    STORE T2, T0, T1
    LOAD A0, T0, T1
    BNE A0, T2, L_fail
    ADDI S0, S0, -1
    BNE S0, R0, L_loop
    LI A0, 89
    STORE A0, R0, R0
    LI RV0, 0
    HALT
L_fail:
    LI A0, 78
    STORE A0, R0, R0
    LI RV0, 1
    HALT
