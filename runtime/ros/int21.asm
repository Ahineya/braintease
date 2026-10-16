; unsigned ros_int21(unsigned ah, unsigned a1, unsigned a2, unsigned a3)
; Args already in A0–A3 from the C CALL.
;
; IRET resumes after the STATUS store; RA/RAB keep ordinary CALL semantics.
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
