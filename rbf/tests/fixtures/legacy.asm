; Two pixels in the existing mode-3 front/back layout.
_start:
    LI T0, 513
    LI T1, 16
    STORE T0, R0, T1
    LI T0, 3
    LI T1, 6
    STORE T0, R0, T1
    LI T0, 63488
    LI T1, 34
    STORE T0, R0, T1
    LI T0, 31
    LI T1, 35
    STORE T0, R0, T1
    LI T0, 1
    LI T1, 9
    STORE T0, R0, T1
    HALT
