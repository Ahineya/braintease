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
