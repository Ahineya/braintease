; TEXT40 smoke: first two cells and bottom-right cell retain distinct attributes.
; Assemble with BANK_SIZE=2048, then export with rlink --bf-host.
; One enabled flush produces exactly 1000 little-endian words:
; cells[0]=0x1e41, cells[1]=0x2f42, cells[999]=0x4f5a; all others zero.
; Interactive terminal: press Z to exit after inspecting the colored cells.
.code
start:
    LI T0, 2
    LI T1, 6
    STORE T0, R0, T1
    LI T0, 3
    LI T1, 8
    STORE T0, R0, T1
    LI T0, 0x1e41
    LI T1, 32
    STORE T0, R0, T1
    LI T0, 0x2f42
    ADDI T1, T1, 1
    STORE T0, R0, T1
    LI T0, 0x4f5a
    LI T1, 1031
    STORE T0, R0, T1
    LI T0, 1
    LI T1, 9
    STORE T0, R0, T1
    LI T1, 14
_wait_for_z:
    LOAD T0, R0, T1
    BEQ T0, R0, _wait_for_z
    HALT
