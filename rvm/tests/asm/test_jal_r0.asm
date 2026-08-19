; JAL R0 is a non-linking jump: RET from the target must return to CALL's caller.

_start:
    CALL through_vec
    LI R5, 66
    STORE R5, R0, R0
    LI R5, 10
    STORE R5, R0, R0
    HALT

through_vec:
    JAL R0, R0, real_fn
    LI R5, 88
    STORE R5, R0, R0
    RET

real_fn:
    LI R5, 65
    STORE R5, R0, R0
    RET
