; unsigned ros_int21(unsigned ah, unsigned a1, unsigned a2, unsigned a3)
; Args already in A0–A3 from the C CALL.
;
; IRQ dispatch writes the *current* PC into RA/RAB (the insn has not run
; yet), same as JAL. Raising IRQ and then RET would therefore RET to that
; RET forever. Save the C return first, interrupt a NOP, then RET to C.
ros_int21:
    STORE RA, SB, SP
    ADDI SP, SP, 1
    STORE RAB, SB, SP
    ADDI SP, SP, 1
    LI T0, 1
    LI T1, 21
    STORE T0, R0, T1
    NOP
    ADDI SP, SP, -1
    LOAD RAB, SB, SP
    ADDI SP, SP, -1
    LOAD RA, SB, SP
    RET
