; SPDX-License-Identifier: GPL-2.0-only
; Fixed physical syscall vector at bank 0, instruction 0. _start is instruction 1.
    BEQ R0, R0, rbf_gate
; SPDX-License-Identifier: GPL-2.0-only
; Physical boot profile: kernel GP=1, kernel stack=2, init argument RAM=3.
_start:
    LI GP, 1
    LI SB, 2
    LI SP, 1
    LI FP, 1
    CALL _init_globals
    CALL main
    HALT
_exit:
    HALT
rbf_stop:
    ADD RV0, A0, R0
    HALT
; IRQ polling is absent from this boot CPU. No asynchronous driver is linked.
__hard_di:
    LI RV0, 0
    RET
__hard_irqrestore:
__hard_ei:
    RET

; The bootstrap never resumes after entering /init. Its stack can be reused by
; synchronous syscalls; IRQs and nested traps are absent from this profile.
rbf_enter:
    ADD T2, A0, R0
    ADD T3, A1, R0
    LI T0, rbf_argc
    LOAD A0, GP, T0
    LI T0, rbf_argv
    LOAD A1, GP, T0
    LI A2, 4
    LI GP, 3
    LI SB, 4
    LI SP, 1
    LI FP, 1
    JALR R0, T2, T3
; Bank 5 becomes disposable after its snapshot has been copied back to bank 3.
rbf_restore_parent:
    LI SB, 5
    LI SP, 1
    LI FP, 1
    CALL rbf_restore_kernel
    HALT
rbf_userptr:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    LI T0, -1
    BNE A1, T0, rbf_ptr_stack
    LI RV1, 3
    RET
rbf_ptr_stack:
    LI T0, -2
    BNE A1, T0, rbf_ptr_zero
    LI RV1, 4
    RET
rbf_ptr_zero:
    BNE A0, R0, rbf_ptr_done
    BNE A1, R0, rbf_ptr_done
    LI RV1, -3
rbf_ptr_done:
    RET
rbf_gate:
    LI T0, -1
    BNE A1, T0, rbf_gate_stack
    ADD A1, GP, R0
rbf_gate_stack:
    LI T0, -2
    BNE A1, T0, rbf_gate_save
    ADD A1, SB, R0
rbf_gate_save:
    ADD T0, SP, R0
    ADD T1, FP, R0
    ADD T2, RA, R0
    ADD T3, RAB, R0
    ADD T4, GP, R0
    ADD T5, SB, R0
    LI GP, 1
    LI SB, 2
    LI SP, 1
    STORE T0, SB, SP
    ADDI SP, SP, 1
    STORE T1, SB, SP
    ADDI SP, SP, 1
    STORE T2, SB, SP
    ADDI SP, SP, 1
    STORE T3, SB, SP
    ADDI SP, SP, 1
    STORE T4, SB, SP
    ADDI SP, SP, 1
    STORE T5, SB, SP
    ADDI SP, SP, 1
    STORE S0, SB, SP
    ADDI SP, SP, 1
    STORE S1, SB, SP
    ADDI SP, SP, 1
    STORE S2, SB, SP
    ADDI SP, SP, 1
    STORE S3, SB, SP
    ADDI SP, SP, 1
    ADD FP, SP, R0
    CALL rbf_dispatch
    ADDI SP, SP, -1
    LOAD S3, SB, SP
    ADDI SP, SP, -1
    LOAD S2, SB, SP
    ADDI SP, SP, -1
    LOAD S1, SB, SP
    ADDI SP, SP, -1
    LOAD S0, SB, SP
    ADDI SP, SP, -1
    LOAD T5, SB, SP
    ADDI SP, SP, -1
    LOAD T4, SB, SP
    ADDI SP, SP, -1
    LOAD T3, SB, SP
    ADDI SP, SP, -1
    LOAD T2, SB, SP
    ADDI SP, SP, -1
    LOAD T1, SB, SP
    ADDI SP, SP, -1
    LOAD T0, SB, SP
    ADD SP, T0, R0
    ADD FP, T1, R0
    ADD RA, T2, R0
    ADD RAB, T3, R0
    ADD GP, T4, R0
    ADD SB, T5, R0
    RET

rbf_context_save:
    ADD T1, A0, R0
    LI T0, 1
    STORE SP, T0, T1
    ADDI T1, T1, 1
    STORE FP, T0, T1
    ADDI T1, T1, 1
    STORE SB, T0, T1
    ADDI T1, T1, 1
    STORE RA, T0, T1
    ADDI T1, T1, 1
    STORE RAB, T0, T1
    ADDI T1, T1, 1
    STORE S0, T0, T1
    ADDI T1, T1, 1
    STORE S1, T0, T1
    ADDI T1, T1, 1
    STORE S2, T0, T1
    ADDI T1, T1, 1
    STORE S3, T0, T1
    ADDI T1, T1, 1
    LI RV0, 0
    RET

rbf_context_restore:
    ADD T1, A0, R0
    LI T0, 1
    LOAD SP, T0, T1
    ADDI T1, T1, 1
    LOAD FP, T0, T1
    ADDI T1, T1, 1
    LOAD SB, T0, T1
    ADDI T1, T1, 1
    LOAD RA, T0, T1
    ADDI T1, T1, 1
    LOAD RAB, T0, T1
    ADDI T1, T1, 1
    LOAD S0, T0, T1
    ADDI T1, T1, 1
    LOAD S1, T0, T1
    ADDI T1, T1, 1
    LOAD S2, T0, T1
    ADDI T1, T1, 1
    LOAD S3, T0, T1
    ADDI T1, T1, 1
    LI RV0, 1
    RET
