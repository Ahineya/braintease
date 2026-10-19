; SPDX-License-Identifier: GPL-2.0-only
_start:
    STORE A0, SB, SP
    ADDI SP, SP, 1
    STORE A1, SB, SP
    ADDI SP, SP, 1
    STORE A2, SB, SP
    ADDI SP, SP, 1
    CALL _init_globals
    ADDI SP, SP, -1
    LOAD A2, SB, SP
    ADDI SP, SP, -1
    LOAD A1, SB, SP
    ADDI SP, SP, -1
    LOAD A0, SB, SP
    CALL main
    ADD A0, RV0, R0
    CALL rbf_user_exit
    HALT
; Same request layout as the maintained port, synchronous ordinary-ISA call.
ripple_syscall:
    STORE RA, SB, SP
    ADDI SP, SP, 1
    STORE RAB, SB, SP
    ADDI SP, SP, 1
    JALR RA, R0, R0
    ADDI SP, SP, -1
    LOAD RAB, SB, SP
    ADDI SP, SP, -1
    LOAD RA, SB, SP
    RET
