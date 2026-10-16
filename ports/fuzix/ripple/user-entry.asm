_start:
    STORE A0, SB, SP
    ADDI SP, SP, 1
    STORE A1, SB, SP
    ADDI SP, SP, 1
    STORE A2, SB, SP
    ADDI SP, SP, 1
    STORE A3, SB, SP
    ADDI SP, SP, 1
    CALL _init_globals
    LI T0, 2
    LOAD A0, SB, T0
    ADDI T0, T0, 1
    LOAD A1, SB, T0
    ADDI T0, T0, 1
    LOAD A2, SB, T0
    ADDI T0, T0, 1
    LOAD A3, SB, T0
    LI SP, 2
    LI X2, 1
    CALL main
    CALL user_exit
    HALT
_exit:
    CALL user_exit
    HALT
ripple_syscall:
    STORE RA, SB, SP
    ADDI SP, SP, 1
    STORE RAB, SB, SP
    ADDI SP, SP, 1
    LI T0, 1
    LI T1, 0
    JALR RA, T0, T1
    ADDI SP, SP, -1
    LOAD RAB, SB, SP
    ADDI SP, SP, -1
    LOAD RA, SB, SP
    RET
