; SPDX-License-Identifier: GPL-2.0-or-later
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
    LI X2, 1
    LI T0, 1
    LOAD T1, SB, T0
    STORE T1, SB, SP
    ADDI SP, SP, 1
    STORE A3, SB, SP
    ADDI SP, SP, 1
    CALL ripple_libc_init
    ADDI SP, SP, -2
    LI T0, 2
    LOAD A0, SB, T0
    ADDI T0, T0, 1
    LOAD A1, SB, T0
    ADDI T0, T0, 1
    LOAD A2, SB, T0
    ADDI T0, T0, 1
    LOAD A3, SB, T0
    LI T0, 1
    LOAD T1, SB, T0
    STORE T1, SB, SP
    ADDI SP, SP, 1
    STORE A3, SB, SP
    ADDI SP, SP, 1
    CALL main
    ADDI SP, SP, -2
    ADD A0, RV0, R0
    CALL exit
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

; RCC non-local jumps preserve the full callee state and concrete pointer bank.
setjmp:
    LI T0, -1
    BNE A1, T0, setjmp_sb
    ADD A1, GP, R0
setjmp_sb:
    LI T0, -2
    BNE A1, T0, setjmp_save
    ADD A1, SB, R0
setjmp_save:
    STORE SP, A1, A0
    ADDI A0, A0, 1
    STORE FP, A1, A0
    ADDI A0, A0, 1
    STORE SB, A1, A0
    ADDI A0, A0, 1
    STORE RA, A1, A0
    ADDI A0, A0, 1
    STORE RAB, A1, A0
    ADDI A0, A0, 1
    STORE S0, A1, A0
    ADDI A0, A0, 1
    STORE S1, A1, A0
    ADDI A0, A0, 1
    STORE S2, A1, A0
    ADDI A0, A0, 1
    STORE S3, A1, A0
    ADDI A0, A0, 1
    STORE GP, A1, A0
    LI RV0, 0
    RET
longjmp:
    LI T0, -1
    BNE A1, T0, longjmp_sb
    ADD A1, GP, R0
longjmp_sb:
    LI T0, -2
    BNE A1, T0, longjmp_restore
    ADD A1, SB, R0
longjmp_restore:
    ADD RV0, A2, R0
    BNE RV0, R0, longjmp_nonzero
    LI RV0, 1
longjmp_nonzero:
    LOAD SP, A1, A0
    ADDI A0, A0, 1
    LOAD FP, A1, A0
    ADDI A0, A0, 1
    LOAD SB, A1, A0
    ADDI A0, A0, 1
    LOAD RA, A1, A0
    ADDI A0, A0, 1
    LOAD RAB, A1, A0
    ADDI A0, A0, 1
    LOAD S0, A1, A0
    ADDI A0, A0, 1
    LOAD S1, A1, A0
    ADDI A0, A0, 1
    LOAD S2, A1, A0
    ADDI A0, A0, 1
    LOAD S3, A1, A0
    ADDI A0, A0, 1
    LOAD GP, A1, A0
    RET

; The kernel placed a complete interrupted register frame below SP.
ripple_signal_entry:
    STORE A3, SB, SP
    ADDI SP, SP, 1
    JALR RA, A2, A1
    ADDI SP, SP, -1
    LOAD A0, SB, SP
    LI A1, 9
    CALL ripple_sigreturn
    HALT
