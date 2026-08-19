; NOP is 0x42 and must not halt. HALT is 0x00.

_start:
    NOP
    LI R5, 89
    STORE R5, R0, R0
    NOP
    LI R5, 10
    STORE R5, R0, R0
    HALT
