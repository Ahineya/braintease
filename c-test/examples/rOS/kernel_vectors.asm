; Must be the first object in KERNEL.SYS.

k_mount_vec:
    JAL R0, R0, fat16_mount
k_lookup_vec:
    JAL R0, R0, fat16_lookup
k_resolve_vec:
    JAL R0, R0, fat16_resolve
k_dir_open_vec:
    JAL R0, R0, fat16_dir_open
k_dir_next_vec:
    JAL R0, R0, fat16_dir_next
k_read_at_vec:
    JAL R0, R0, fat16_read_at
k_exec_vec:
    JAL R0, R0, k_exec
k_exit_vec:
    JAL R0, R0, k_exit

; Instruction 8 — IRQ vector. Keep this at K_SLOT_INT21.
; IRQ already stored user resume in RA/RAB. Switch to KERNEL GP before C.
int21_handler:
    STORE RA, SB, SP
    ADDI SP, SP, 1
    STORE RAB, SB, SP
    ADDI SP, SP, 1
    STORE GP, SB, SP
    ADDI SP, SP, 1
    LI GP, 4
    CALL int21_dispatch
    LI T0, 1
    LI T1, 23
    STORE T0, R0, T1
    ADDI SP, SP, -1
    LOAD GP, SB, SP
    ADDI SP, SP, -1
    LOAD RAB, SB, SP
    ADDI SP, SP, -1
    LOAD RA, SB, SP
    RET
