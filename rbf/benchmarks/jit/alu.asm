; Native-word multiplication, positive signed DIV/MOD, XOR and logical shifts.
; Values retain high bits; division uses x>>1 to stay in the positive domain.
start:
    LI T0, 12345
    LI S0, 0
    LI S1, 64
loop:
    MULI T0, T0, 109
    ADDI T0, T0, 89
    SRLI T3, T0, 1
    DIVI T1, T3, 37
    MODI T2, T3, 37
    XOR T1, T1, T2
    ADD S0, S0, T1
    SRLI T2, T0, 5
    XOR S0, S0, T2
    ADDI S1, S1, -1
    BNE S1, R0, loop
    LI T1, 33804 ; Independently calculated 16-bit checksum.
    BNE S0, T1, fail
    LI A0, 89
    STORE A0, R0, R0
    LI RV0, 0
    HALT
fail:
    LI A0, 78
    STORE A0, R0, R0
    LI RV0, 1
    HALT
