; crt0 for RXE apps. main return / _exit is INT21 AH=0x4C (does not HALT).

_start:
    CALL _init_globals
    CALL main
    ADD A1, RV0, R0
    LI A0, 0x4C
    CALL ros_int21
    HALT

_exit:
    ADD A1, A0, R0
    LI A0, 0x4C
    CALL ros_int21
    HALT
