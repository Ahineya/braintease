; Ripple FUZIX kernel entry and syscall vector. Kernel IMEM starts at bank 1.
; Protected userspace always uses virtual stack bank 31 (USER_SB in mmu.h).
ripple_syscall_vector:
    BEQ R0, R0, ripple_syscall_entry
_start:
    CALL _init_globals
    CALL main
    HALT
_exit:
ripple_halt:
    HALT
__hard_di:
    LI T0, 22
    LOAD RV0, R0, T0
    STORE R0, R0, T0
    RET
__hard_ei:
    LI T0, 22
    LI T1, 2
    STORE T1, R0, T0
    RET
__hard_irqrestore:
    LI T0, 22
    STORE A0, R0, T0
    RET
; copy an entire DMEM bank; fixed execution slots preserve embedded pointers.
ripple_bank_copy_slow:
    LI T0, 0
    LI T2, 64000
ripple_copy_loop:
    LOAD T1, A0, T0
    STORE T1, A1, T0
    ADDI T0, T0, 1
    BNE T0, T2, ripple_copy_loop
    RET
ripple_context_save:
    LI T0, 3
    ADD T1, A0, R0
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
ripple_context_restore:
    LI T0, 3
    ADD T1, A0, R0
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
; A0/A1 = request pointer. Resolve user-relative tags before changing GP/SB.
ripple_syscall_entry:
    LI T0, 22
    STORE R0, R0, T0
    LI T0, -1
    BNE A1, T0, ripple_sys_sb_tag
    ADD A1, GP, R0
ripple_sys_sb_tag:
    LI T0, -2
    BNE A1, T0, ripple_sys_resolved
    ADD A1, SB, R0
ripple_sys_resolved:
    ADD T2, SB, R0
    ADD T3, SP, R0
    ADD T4, FP, R0
    ADD T5, GP, R0
    LI GP, 3
    LI T0, ripple_kernel_stack
    LOAD SB, GP, T0
    LI SP, 1
    STORE R3, SB, SP
    ADDI SP, SP, 1
    STORE R4, SB, SP
    ADDI SP, SP, 1
    STORE R5, SB, SP
    ADDI SP, SP, 1
    STORE R6, SB, SP
    ADDI SP, SP, 1
    STORE R7, SB, SP
    ADDI SP, SP, 1
    STORE R8, SB, SP
    ADDI SP, SP, 1
    STORE R9, SB, SP
    ADDI SP, SP, 1
    STORE R10, SB, SP
    ADDI SP, SP, 1
    STORE R11, SB, SP
    ADDI SP, SP, 1
    STORE R12, SB, SP
    ADDI SP, SP, 1
    STORE R13, SB, SP
    ADDI SP, SP, 1
    STORE R14, SB, SP
    ADDI SP, SP, 1
    STORE R15, SB, SP
    ADDI SP, SP, 1
    STORE R16, SB, SP
    ADDI SP, SP, 1
    STORE R17, SB, SP
    ADDI SP, SP, 1
    STORE R18, SB, SP
    ADDI SP, SP, 1
    STORE R19, SB, SP
    ADDI SP, SP, 1
    STORE R20, SB, SP
    ADDI SP, SP, 1
    STORE R21, SB, SP
    ADDI SP, SP, 1
    STORE R22, SB, SP
    ADDI SP, SP, 1
    STORE R23, SB, SP
    ADDI SP, SP, 1
    STORE R24, SB, SP
    ADDI SP, SP, 1
    STORE R25, SB, SP
    ADDI SP, SP, 1
    STORE R26, SB, SP
    ADDI SP, SP, 1
    STORE R27, SB, SP
    ADDI SP, SP, 1
    STORE T5, SB, SP
    ADDI SP, SP, 1
    STORE RA, SB, SP
    ADDI SP, SP, 1
    STORE RAB, SB, SP
    ADDI SP, SP, 1
    LI T0, 2
    STORE T0, SB, SP
    ADDI SP, SP, 1
    STORE T2, SB, SP
    ADDI SP, SP, 1
    STORE T3, SB, SP
    ADDI SP, SP, 1
    STORE T4, SB, SP
    ADDI SP, SP, 1
    ADD FP, SP, R0
    CALL ripple_dispatch
    LI T0, 3
    STORE RV0, SB, T0
    ADDI T0, T0, 1
    STORE RV1, SB, T0
    CALL ripple_signal_frame
    BNE RV0, R0, ripple_resume_user
    LI T0, 30
    LOAD T2, SB, T0
    ADDI T0, T0, 1
    LOAD T3, SB, T0
    ADDI T0, T0, 1
    LOAD T4, SB, T0
    LI SP, 27
    ADDI SP, SP, -1
    LOAD GP, SB, SP
    ADDI SP, SP, -1
    LOAD R27, SB, SP
    ADDI SP, SP, -1
    LOAD R26, SB, SP
    ADDI SP, SP, -1
    LOAD R25, SB, SP
    ADDI SP, SP, -1
    LOAD R24, SB, SP
    ADDI SP, SP, -1
    LOAD R23, SB, SP
    ADDI SP, SP, -1
    LOAD R22, SB, SP
    ADDI SP, SP, -1
    LOAD R21, SB, SP
    ADDI SP, SP, -1
    LOAD R20, SB, SP
    ADDI SP, SP, -1
    ADDI SP, SP, -1
    ADDI SP, SP, -1
    ADDI SP, SP, -1
    LOAD R16, SB, SP
    ADDI SP, SP, -1
    LOAD R15, SB, SP
    ADDI SP, SP, -1
    LOAD R14, SB, SP
    ADDI SP, SP, -1
    LOAD R13, SB, SP
    ADDI SP, SP, -1
    LOAD R12, SB, SP
    ADDI SP, SP, -1
    LOAD R11, SB, SP
    ADDI SP, SP, -1
    LOAD R10, SB, SP
    ADDI SP, SP, -1
    LOAD R9, SB, SP
    ADDI SP, SP, -1
    LOAD R8, SB, SP
    ADDI SP, SP, -1
    LOAD R7, SB, SP
    ADDI SP, SP, -1
    LOAD R6, SB, SP
    ADDI SP, SP, -1
    LOAD R5, SB, SP
    ADDI SP, SP, -1
    LOAD R4, SB, SP
    ADDI SP, SP, -1
    LOAD R3, SB, SP
    ADD FP, T4, R0
    ADD SP, T3, R0
    ADD SB, T2, R0
    LI T0, 22
    LI T1, 2
    STORE T1, R0, T0
    RET
; User startup receives argc, argv and envp in RCC argument slots.
ripple_enter_user:
    LI T0, ripple_exec_bank
    LOAD T6, GP, T0
    LI T0, ripple_exec_pc
    LOAD T7, GP, T0
    LI T0, ripple_argc
    LOAD A0, GP, T0
    LI T0, ripple_argv
    LOAD A1, GP, T0
    ADDI T0, T0, 1
    LOAD A2, GP, T0
    LI T0, ripple_envp
    LOAD A3, GP, T0
    ADDI T0, T0, 1
    LOAD T1, GP, T0
    LI SB, 31
    LI SP, 1
    STORE T1, SB, SP
    ADDI SP, SP, 1
    LI FP, 1
    LI GP, 8
    LI X2, 1
    LI T0, 22
    LI T1, 2
    STORE T1, R0, T0
    ENTERU R0, T6, T7
; Hardware switches to the dedicated IRQ stack; IRET restores SP/SB/FP.
ripple_irq:
    STORE R3, SB, SP
    ADDI SP, SP, 1
    STORE R4, SB, SP
    ADDI SP, SP, 1
    STORE R5, SB, SP
    ADDI SP, SP, 1
    STORE R6, SB, SP
    ADDI SP, SP, 1
    STORE R7, SB, SP
    ADDI SP, SP, 1
    STORE R8, SB, SP
    ADDI SP, SP, 1
    STORE R9, SB, SP
    ADDI SP, SP, 1
    STORE R10, SB, SP
    ADDI SP, SP, 1
    STORE R11, SB, SP
    ADDI SP, SP, 1
    STORE R12, SB, SP
    ADDI SP, SP, 1
    STORE R13, SB, SP
    ADDI SP, SP, 1
    STORE R14, SB, SP
    ADDI SP, SP, 1
    STORE R15, SB, SP
    ADDI SP, SP, 1
    STORE R16, SB, SP
    ADDI SP, SP, 1
    STORE R17, SB, SP
    ADDI SP, SP, 1
    STORE R18, SB, SP
    ADDI SP, SP, 1
    STORE R19, SB, SP
    ADDI SP, SP, 1
    STORE R20, SB, SP
    ADDI SP, SP, 1
    STORE R21, SB, SP
    ADDI SP, SP, 1
    STORE R22, SB, SP
    ADDI SP, SP, 1
    STORE R23, SB, SP
    ADDI SP, SP, 1
    STORE R24, SB, SP
    ADDI SP, SP, 1
    STORE R25, SB, SP
    ADDI SP, SP, 1
    STORE R26, SB, SP
    ADDI SP, SP, 1
    STORE R27, SB, SP
    ADDI SP, SP, 1
    STORE GP, SB, SP
    ADDI SP, SP, 1
    LI GP, 3
    CSRR T0, 22
    BNE T0, R0, ripple_exception_interrupt
    LI T0, 26
    LOAD T1, R0, T0
    LI T0, 1
    BEQ T1, T0, ripple_restore_interrupt
    ADD FP, SP, R0
    CALL ripple_tick
    BNE RV0, R0, ripple_preempt_interrupt
ripple_irq_restore_registers:
    ADDI SP, SP, -1
    LOAD GP, SB, SP
    ADDI SP, SP, -1
    LOAD R27, SB, SP
    ADDI SP, SP, -1
    LOAD R26, SB, SP
    ADDI SP, SP, -1
    LOAD R25, SB, SP
    ADDI SP, SP, -1
    LOAD R24, SB, SP
    ADDI SP, SP, -1
    LOAD R23, SB, SP
    ADDI SP, SP, -1
    LOAD R22, SB, SP
    ADDI SP, SP, -1
    LOAD R21, SB, SP
    ADDI SP, SP, -1
    LOAD R20, SB, SP
    ADDI SP, SP, -1
    LOAD R19, SB, SP
    ADDI SP, SP, -1
    LOAD R18, SB, SP
    ADDI SP, SP, -1
    LOAD R17, SB, SP
    ADDI SP, SP, -1
    LOAD R16, SB, SP
    ADDI SP, SP, -1
    LOAD R15, SB, SP
    ADDI SP, SP, -1
    LOAD R14, SB, SP
    ADDI SP, SP, -1
    LOAD R13, SB, SP
    ADDI SP, SP, -1
    LOAD R12, SB, SP
    ADDI SP, SP, -1
    LOAD R11, SB, SP
    ADDI SP, SP, -1
    LOAD R10, SB, SP
    ADDI SP, SP, -1
    LOAD R9, SB, SP
    ADDI SP, SP, -1
    LOAD R8, SB, SP
    ADDI SP, SP, -1
    LOAD R7, SB, SP
    ADDI SP, SP, -1
    LOAD R6, SB, SP
    ADDI SP, SP, -1
    LOAD R5, SB, SP
    ADDI SP, SP, -1
    LOAD R4, SB, SP
    ADDI SP, SP, -1
    LOAD R3, SB, SP
    IRET
ripple_exception_interrupt:
    ADD FP, SP, R0
    CALL ripple_fault
    BEQ RV0, R0, ripple_irq_restore_registers
ripple_preempt_interrupt:
    CSRR T0, 0
    STORE T0, SB, SP
    ADDI SP, SP, 1
    CSRR T0, 1
    STORE T0, SB, SP
    ADDI SP, SP, 1
    CSRR T0, 2
    STORE T0, SB, SP
    ADDI SP, SP, 1
    CSRR T0, 7
    STORE T0, SB, SP
    ADDI SP, SP, 1
    CSRR T0, 8
    STORE T0, SB, SP
    ADDI SP, SP, 1
    CSRR T0, 9
    STORE T0, SB, SP
    ADDI SP, SP, 1
    LI T0, ripple_kernel_stack
    LOAD T1, GP, T0
    LI T0, 1
    LI T3, 33
    LI T4, 4
ripple_preempt_copy:
    LOAD T2, T4, T0
    STORE T2, T1, T0
    ADDI T0, T0, 1
    BNE T0, T3, ripple_preempt_copy
    CSRW T1, 7
    CSRW T3, 8
    CSRW T3, 9
    CSRR T0, 22
    BEQ T0, R0, ripple_choose_preempt
    LI T0, %offset(ripple_trap_entry)
    CSRW T0, 0
    LI T0, %bank(ripple_trap_entry)
    CSRW T0, 1
    BEQ R0, R0, ripple_trampoline_selected
ripple_choose_preempt:
    LI T0, %offset(ripple_preempt_entry)
    CSRW T0, 0
    LI T0, %bank(ripple_preempt_entry)
    CSRW T0, 1
ripple_trampoline_selected:
    CSRW R0, 25
    CSRW R0, 2
    LI T0, 23
    LI T1, 1
    STORE T1, R0, T0
    IRET
; CPU has completed IRET: IRQ_BUSY is clear before any scheduler can block.
ripple_trap_entry:
    LI X2, 0
    CALL ripple_trap
    HALT
ripple_preempt_entry:
    LI X2, 0
    CALL ripple_preempt
    HALT
ripple_resume_user:
    LI T0, 22
    STORE R0, R0, T0
    LI T1, 1
    LI T0, 21
    STORE T1, R0, T0
    LI T0, 22
    STORE T1, R0, T0
ripple_resume_wait:
    BEQ R0, R0, ripple_resume_wait
; A software IRQ provides a real CPU trap frame for restoring all registers.
ripple_restore_interrupt:
    LI T0, 1
    CSRW T0, 25
    LI T0, ripple_kernel_stack
    LOAD T1, GP, T0
    LI T0, 27
    LOAD T2, T1, T0
    CSRW T2, 0
    ADDI T0, T0, 1
    LOAD T2, T1, T0
    CSRW T2, 1
    ADDI T0, T0, 1
    LOAD T2, T1, T0
    CSRW T2, 2
    ADDI T0, T0, 1
    LOAD T2, T1, T0
    CSRW T2, 7
    ADDI T0, T0, 1
    LOAD T2, T1, T0
    CSRW T2, 8
    ADDI T0, T0, 1
    LOAD T2, T1, T0
    CSRW T2, 9
    ADDI T0, T0, 1
    LI T0, 23
    LI T2, 1
    STORE T2, R0, T0
    ADD SB, T1, R0
    LI SP, 27
    ADDI SP, SP, -1
    LOAD GP, SB, SP
    ADDI SP, SP, -1
    LOAD R27, SB, SP
    ADDI SP, SP, -1
    LOAD R26, SB, SP
    ADDI SP, SP, -1
    LOAD R25, SB, SP
    ADDI SP, SP, -1
    LOAD R24, SB, SP
    ADDI SP, SP, -1
    LOAD R23, SB, SP
    ADDI SP, SP, -1
    LOAD R22, SB, SP
    ADDI SP, SP, -1
    LOAD R21, SB, SP
    ADDI SP, SP, -1
    LOAD R20, SB, SP
    ADDI SP, SP, -1
    LOAD R19, SB, SP
    ADDI SP, SP, -1
    LOAD R18, SB, SP
    ADDI SP, SP, -1
    LOAD R17, SB, SP
    ADDI SP, SP, -1
    LOAD R16, SB, SP
    ADDI SP, SP, -1
    LOAD R15, SB, SP
    ADDI SP, SP, -1
    LOAD R14, SB, SP
    ADDI SP, SP, -1
    LOAD R13, SB, SP
    ADDI SP, SP, -1
    LOAD R12, SB, SP
    ADDI SP, SP, -1
    LOAD R11, SB, SP
    ADDI SP, SP, -1
    LOAD R10, SB, SP
    ADDI SP, SP, -1
    LOAD R9, SB, SP
    ADDI SP, SP, -1
    LOAD R8, SB, SP
    ADDI SP, SP, -1
    LOAD R7, SB, SP
    ADDI SP, SP, -1
    LOAD R6, SB, SP
    ADDI SP, SP, -1
    LOAD R5, SB, SP
    ADDI SP, SP, -1
    LOAD R4, SB, SP
    ADDI SP, SP, -1
    LOAD R3, SB, SP
    IRET

; Low-level IMEM/DMEM helpers. No C prologue — keep A0–A3 intact.

; void imem_store(unsigned bank, unsigned addr, unsigned *cells)
; A0=bank A1=addr A2=cells_addr A3=cells_bank
; C fat-pointer bank tags: -1 = GP, -2 = SB. A naked LOAD uses the raw
; number, so resolve tags here or STORC writes garbage (and the guest HALTs).
imem_store:
    LI T1, -1
    BNE A3, T1, imem_chk_sb
    ADD A3, GP, R0
    BEQ R0, R0, imem_do
imem_chk_sb:
    LI T1, -2
    BNE A3, T1, imem_do
    ADD A3, SB, R0
imem_do:
    LOAD X0, A3, A2
    ADDI T0, A2, 1
    LOAD X1, A3, T0
    ADDI T0, A2, 2
    LOAD X2, A3, T0
    ADDI T0, A2, 3
    LOAD X3, A3, T0
    STORC R0, A0, A1
    RET

; unsigned dmem_load(unsigned bank, unsigned addr)
dmem_load:
    LOAD RV0, A0, A1
    RET

; void dmem_store(unsigned bank, unsigned addr, unsigned value)
dmem_store:
    STORE A2, A0, A1
    RET


; Inspect incoming pointer tags BEFORE C's pointer-to-integer conversion,
; which materializes relative tags against the current (kernel) GP/SB.
ripple_userptr:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    LI T0, -1
    BNE A1, T0, ripple_ptr_stack
    LI RV1, 8
    RET
ripple_ptr_stack:
    LI T0, -2
    BNE A1, T0, ripple_ptr_zero
    LI RV1, 31
    RET
ripple_ptr_zero:
    BNE A0, R0, ripple_ptr_done
    BNE A1, R0, ripple_ptr_done
    LI RV1, -3
ripple_ptr_done:
    RET

; A0 is the physical table bank; A1 is this task's table offset within it.
ripple_mmu_roots:
    CSRW A0, 14
    CSRW A0, 16
    CSRW A1, 15
    LI T0, 1536
    ADD T0, T0, A1
    CSRW T0, 17
    LI T0, 8
    CSRW T0, 18
    LI T0, 31
    CSRW T0, 19
    LI T0, %bank(ripple_irq)
    CSRW T0, 20
    LI T0, %offset(ripple_irq)
    CSRW T0, 21
    LI T0, 3
    CSRW T0, 13
    RET
ripple_mmu_flush:
    LI T0, 1
    CSRW T0, 27
    RET
ripple_mmu_flush_code:
    LI T0, 3
    CSRW T0, 27
    RET
ripple_mmu_cause:
    CSRR RV0, 22
    RET
ripple_mmu_fault_bank:
    CSRR RV0, 23
    RET
ripple_mmu_fault_off:
    CSRR RV0, 24
    RET
ripple_mmu_saved_mode:
    CSRR RV0, 25
    RET
