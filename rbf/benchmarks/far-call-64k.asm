; BANK_SIZE=64000 MEMORY_BANKS=256
; Alternate late-code calls with bank-136 LOAD/STORE operations. This combines
; instruction-marker and RAM-marker traversals that isolated fixtures miss.
start:
    LI S0, 16
    LI S1, 40000
    LI S3, 0
    LI T0, 136
    LI T1, 52000
    LI T3, %bank(worker)
    LI T4, %offset(worker)
L_loop:
    STORE S1, T0, T1
    LOAD A0, T0, T1
    BNE A0, S1, L_fail
    JALR RA, T3, T4
    BNE S3, R0, L_fail
    ADDI S1, S1, 1
    LOAD A0, T0, T1
    BNE A0, S1, L_fail
    ADDI T1, T1, 1
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

; The first full segment starts at bank 1 after the caller. The final partial
; segment puts worker at bank 30, offset 6000, without executing the padding.
; BENCH_PADDING 29 64000
; BENCH_PADDING 1 6000
worker:
    LOAD A0, T0, T1
    XOR S3, A0, S1
    ADDI A0, A0, 1
    STORE A0, T0, T1
    RET
