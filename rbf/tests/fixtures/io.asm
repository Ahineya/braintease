; Real BF-CPU read/write boundary check. Use bank size 512, seed 1,
; --input AZ, and a fresh three-byte disk containing 34 12 56 (hex).
; Success emits Y, returns RV0=0, and commits disk prefix 34 12 CD AB.
_start:
    LI T0, 1
    LOAD T1, R0, T0        ; TTY ready
    LI T2, 1
    BNE T1, T2, fail
    LI T0, 3
    LOAD T1, R0, T0        ; input available
    BNE T1, T2, fail
    LI T0, 2
    LOAD T1, R0, T0        ; repeated LOADs must pop different bytes
    LI T2, 65
    BNE T1, T2, fail
    LOAD T1, R0, T0
    LI T2, 90
    BNE T1, T2, fail
    LOAD T1, R0, T0        ; empty input returns zero
    BNE T1, R0, fail
    LI T0, 3
    LOAD T1, R0, T0
    BNE T1, R0, fail

    LI T0, 4
    LOAD T1, R0, T0        ; seeded RNG advances on each read
    LI T2, 15496          ; 0x3C88
    BNE T1, T2, fail
    LOAD T1, R0, T0
    LI T2, 24200          ; 0x5E88
    BNE T1, T2, fail

    LI T0, 17
    STORE R0, R0, T0      ; block zero
    LI T0, 18
    STORE R0, R0, T0      ; word zero
    LI T0, 19
    LOAD T1, R0, T0        ; consecutive reads advance the word cursor
    LI T2, 4660           ; 0x1234
    BNE T1, T2, fail
    LOAD T1, R0, T0
    LI T2, 86             ; partial final word is zero-padded: 0x0056
    BNE T1, T2, fail
    LI T0, 18
    LI T1, 1
    STORE T1, R0, T0      ; reseek the second word
    LI T0, 19
    LI T1, 43981          ; 0xABCD
    STORE T1, R0, T0
    LI T0, 20
    LOAD T1, R0, T0
    LI T2, 2              ; STORE_DIRTY
    BNE T1, T2, fail
    LI T1, 4              ; STORE_COMMIT
    STORE T1, R0, T0
    LOAD T1, R0, T0
    BNE T1, R0, fail
    LI T0, 18
    LI T1, 1
    STORE T1, R0, T0
    LI T0, 19
    LOAD T1, R0, T0        ; committed word remains readable
    LI T2, 43981
    BNE T1, T2, fail

    LI T0, 89
    STORE T0, R0, R0
    LI RV0, 0
    HALT
fail:
    LI T0, 78
    STORE T0, R0, R0
    LI RV0, 1
    HALT
