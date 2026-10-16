; Tiny real BF-CPU device check. Bank size 512; no native CPU shortcuts.
_start:
    LI T0, 4
    LI T1, 6
    STORE T0, R0, T1        ; RGB565_EXT
    LI T0, 1
    LI T1, 60
    STORE T0, R0, T1        ; ABI version
    LI T0, 8
    LI T1, 61
    STORE T0, R0, T1        ; descriptor length
    LI T0, 60
    LI T1, 28
    STORE T0, R0, T1        ; descriptor pointer
    LI T0, 3
    LI T1, 29
    STORE T0, R0, T1        ; one NEXT_TICK
    LI T0, 62
    LOAD T1, R0, T0
    LI T2, 1
    BNE T1, T2, fail
    LI T0, 4
    LI T1, 29
    STORE T0, R0, T1        ; GET_TIME twice must not advance
    STORE T0, R0, T1
    LI T0, 62
    LOAD T1, R0, T0
    BNE T1, T2, fail
    LI T0, 10
    LI T1, 61
    STORE T0, R0, T1        ; INPUT_READ length
    LI T0, 3
    LI T1, 62
    STORE T0, R0, T1        ; capacity 3
    LI T0, 1
    LI T1, 63
    STORE T0, R0, T1        ; event buffer bank 1
    LI T0, 100
    LI T1, 64
    STORE T0, R0, T1        ; event buffer offset
    LI T1, 69
    STORE R0, R0, T1        ; reserved zero
    LI T0, 5
    LI T1, 29
    STORE T0, R0, T1        ; scripted W down at tick 1
    LI T0, 65
    LOAD T1, R0, T0
    LI T2, 3
    BNE T1, T2, fail        ; batch count
    LI T2, 1
    LI T0, 102
    LOAD T1, T2, T0
    LI T3, 26
    BNE T1, T3, fail        ; USB HID W
    LI T0, 101
    LOAD T1, T2, T0
    BNE T1, T2, fail        ; DOWN flag
    LI T0, 120
    LOAD T1, T2, T0
    LI T3, 2
    BNE T1, T3, fail        ; relative motion type
    LI T0, 132
    LOAD T1, T2, T0
    LI T3, 65531
    BNE T1, T3, fail        ; signed dx -5, low word
    LI T0, 133
    LOAD T1, T2, T0
    LI T3, 65535
    BNE T1, T3, fail        ; sign extension
    LI T0, 140
    LOAD T1, T2, T0
    LI T3, 4
    BNE T1, T3, fail        ; wheel type
    LI T0, 159
    LOAD T1, T2, T0
    LI T3, 65535
    BNE T1, T3, fail        ; wheel Y -65536 high word
    LI T0, 5
    LI T1, 29
    STORE T0, R0, T1        ; repeated command executes, queue empty
    LI T0, 65
    LOAD T1, R0, T0
    BNE T1, R0, fail
    LI T0, 8
    LI T1, 61
    STORE T0, R0, T1        ; PRESENT length
    LI T0, 2
    LI T1, 62
    STORE T0, R0, T1        ; width 2
    LI T1, 63
    STORE T0, R0, T1        ; height 2
    LI T0, 3
    LI T1, 64
    STORE T0, R0, T1        ; stride 3
    LI T0, 1
    LI T1, 65
    STORE T0, R0, T1        ; framebuffer bank 1
    LI T0, 510
    LI T1, 66
    STORE T0, R0, T1        ; offset 510, crosses to bank 2
    LI T1, 67
    STORE R0, R0, T1        ; reserved zero
    LI T0, 63488
    LI T1, 510
    LI T2, 1
    STORE T0, T2, T1        ; red
    LI T0, 2016
    LI T1, 511
    STORE T0, T2, T1        ; green
    LI T2, 2
    LI T0, 31
    LI T1, 1
    STORE T0, T2, T1        ; blue
    LI T0, 65535
    LI T1, 2
    STORE T0, T2, T1        ; white
    LI T0, 89
    STORE T0, R0, R0        ; exactly one Y before presentation
    LI T0, 1
    LI T1, 29
    STORE T0, R0, T1        ; PRESENT
    LI RV0, 0
    HALT
fail:
    LI T0, 78
    STORE T0, R0, R0
    LI RV0, 1
    HALT
