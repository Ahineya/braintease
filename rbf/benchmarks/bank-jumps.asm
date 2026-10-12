; BANK_SIZE=257 MEMORY_BANKS=1
; Repeatedly call a distant code bank, using a non-power-of-two bank size.
start:
    LI S0, 32
    LI S1, 0
    LI T0, %bank(worker)
    LI T1, %offset(worker)
L_loop:
    JALR RA, T0, T1
    ADDI S0, S0, -1
    BNE S0, R0, L_loop
    LI T2, 32
    BNE S1, T2, L_fail
    LI A0, 89
    STORE A0, R0, R0
    LI RV0, 0
    HALT
L_fail:
    LI A0, 78
    STORE A0, R0, R0
    LI RV0, 1
    HALT
; BENCH_PADDING 8 257
worker:
    ADDI S1, S1, 1
    RET
