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

; void imem_add_word2(unsigned bank, unsigned addr, unsigned addend)
imem_add_word2:
    LOADC R0, A0, A1
    ADD X2, X2, A2
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

; void ros_enter(unsigned pcb, unsigned pc, unsigned gp, unsigned sb)
; Does not return.
ros_enter:
    ADD GP, A2, R0
    ADD SB, A3, R0
    LI SP, 1
    LI FP, 1
    JALR R0, A0, A1

.data
ctx_gp:
    .space 1
ctx_sb:
    .space 1
ctx_sp:
    .space 1
ctx_fp:
    .space 1
ctx_ra:
    .space 1
ctx_rab:
    .space 1
ctx_rv:
    .space 1

.code

; void ros_save_and_enter(unsigned pcb, unsigned pc, unsigned gp, unsigned sb)
ros_save_and_enter:
    LI T1, 4
    LI T0, ctx_gp
    STORE GP, T1, T0
    LI T0, ctx_sb
    STORE SB, T1, T0
    LI T0, ctx_sp
    STORE SP, T1, T0
    LI T0, ctx_fp
    STORE FP, T1, T0
    LI T0, ctx_ra
    STORE RA, T1, T0
    LI T0, ctx_rab
    STORE RAB, T1, T0
    ADD GP, A2, R0
    ADD SB, A3, R0
    LI SP, 1
    LI FP, 1
    JALR R0, A0, A1

ros_restore:
    LI T0, 4
    ADD GP, T0, R0
    LI T0, ctx_sb
    LOAD SB, GP, T0
    LI T0, ctx_sp
    LOAD SP, GP, T0
    LI T0, ctx_fp
    LOAD FP, GP, T0
    LI T0, ctx_ra
    LOAD RA, GP, T0
    LI T0, ctx_rab
    LOAD RAB, GP, T0
    LI T0, ctx_gp
    LOAD GP, GP, T0
    JALR R0, RAB, RA

ros_set_status:
    LI T0, 4
    LI T1, ctx_rv
    STORE A0, T0, T1
    RET

ros_take_status:
    LI T0, 4
    LI T1, ctx_rv
    LOAD RV0, T0, T1
    RET
