; Software IRQ: raise bit 0, handler prints 'A', ACK, RET; main prints 'Y'

_start:
    LI R6, 25          ; IRQ_VECTOR_OFF
    LI R7, handler
    STORE R7, R0, R6

    LI R6, 22          ; IRQ_ENABLE
    LI R7, 1           ; IRQ_SW
    STORE R7, R0, R6

    LI R6, 21          ; IRQ_STATUS
    STORE R7, R0, R6   ; raise (R7 still 1)

    ; Interrupted here; runs after the handler returns
    LI R5, 89          ; 'Y'
    STORE R5, R0, R0
    LI R5, 10
    STORE R5, R0, R0
    HALT

handler:
    LI R5, 65          ; 'A'
    STORE R5, R0, R0
    LI R6, 23          ; IRQ_BUSY
    LI R7, 1
    STORE R7, R0, R6   ; ACK
    RET
