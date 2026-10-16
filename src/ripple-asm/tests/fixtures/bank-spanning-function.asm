; One function larger than a 37-instruction bank. Two forward/backward
; crossings and nested calls must preserve the original link pair.
start:
    LI S0, 2
    LI S1, 0
    LI RA, 777
    LI RAB, 888
L_loop:
    ADDI S1, S1, 1
    BNE S0, R0, L_far
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
    NOP
    NOP
    NOP
    NOP
L_far:
    MOVE S2, RA
    MOVE S3, RAB
    CALL helper
    MOVE RA, S2
    MOVE RAB, S3
    ADDI S0, S0, -1
    BNE S0, R0, L_loop
    XORI T0, RA, 777
    XORI T1, RAB, 888
    OR T0, T0, T1
    XORI T1, S1, 8
    OR T0, T0, T1
    BNE T0, R0, L_failure
    LI A0, 89
    STORE A0, R0, R0
    HALT
L_failure:
    LI A0, 78
    STORE A0, R0, R0
    HALT
helper:
    ADDI S1, S1, 3
    RET
