; Copy the selected disk words into 16-bit C cells using the existing ISA.
; A0/A1: nonnull canonical/tagged destination; A2: count <=32768.
; Caller has bounded the span to the current storage block. This leaf splits
; at RAM bank boundaries and returns the advanced canonical pointer in RV0/1.
; No callee-saved registers or return-address registers are modified.
.code
Doom_ReadStorageWords:
    LI T0, 65535
    BNE A1, T0, doom_word_read_stack
    ADD A1, GP, R0
doom_word_read_stack:
    LI T0, 65534
    BNE A1, T0, doom_word_read_ready
    ADD A1, SB, R0
doom_word_read_ready:
    BEQ A2, R0, doom_word_read_return
    LI T0, 19
    LI T1, 64000
doom_word_read_chunk:
    SUB T2, T1, A0
    SLTU T3, A2, T2
    BEQ T3, R0, doom_word_read_span
    ADD T2, A2, R0
doom_word_read_span:
    SUB A2, A2, T2
    ADD T2, A0, T2
doom_word_read_loop:
    LOAD T3, R0, T0
    STORE T3, A1, A0
    ADDI A0, A0, 1
    BNE A0, T2, doom_word_read_loop
    BNE A0, T1, doom_word_read_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_word_read_next:
    BNE A2, R0, doom_word_read_chunk
doom_word_read_return:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    JALR R0, RAB, RA
