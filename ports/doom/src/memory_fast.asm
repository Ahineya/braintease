; Port-local C-byte memory routines for BANK_SIZE=64000. C bytes are 16-bit
; cells: memset stores all 16 bits of its int argument, without octet masking.
; All counts are full 32-bit size_t. Each leaf preserves callee-saved registers
; and returns the original destination pointer, including its original tag.
; Pointers must be canonical; global/stack tags are resolved before comparison.
.code

; A0/A1 destination, A2/A3 source, SP-1 count low, SP-2 count high.
Doom_Memcpy:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    BNE T0, R0, doom_copy_nonempty
    BEQ T1, R0, doom_memory_return
doom_copy_nonempty:
    LI T7, 64000
    LI T3, 65535
    BNE A1, T3, doom_copy_dst_stack
    ADD A1, GP, R0
doom_copy_dst_stack:
    LI T3, 65534
    BNE A1, T3, doom_copy_dst_ready
    ADD A1, SB, R0
doom_copy_dst_ready:
    LI T3, 65535
    BNE A3, T3, doom_copy_src_stack
    ADD A3, GP, R0
doom_copy_src_stack:
    LI T3, 65534
    BNE A3, T3, doom_copy_src_ready
    ADD A3, SB, R0
doom_copy_src_ready:
doom_memory_copy_chunk:
    SUB T2, T7, A0
    SUB T3, T7, A2
    SLTU SC, T3, T2
    BEQ SC, R0, doom_memory_copy_source_chunk
    ADD T2, T3, R0
doom_memory_copy_source_chunk:
    BNE T1, R0, doom_memory_copy_count_chunk
    SLTU SC, T0, T2
    BEQ SC, R0, doom_memory_copy_count_chunk
    ADD T2, T0, R0
doom_memory_copy_count_chunk:
    SLTU SC, T0, T2
    SUB T0, T0, T2
    SUB T1, T1, SC
    SRLI T4, T2, 2
    ; remainder = chunk - 4*quotient; all values fit a native word.
    ADD T5, T4, T4
    ADD T5, T5, T5
    SUB T5, T2, T5
    BEQ T4, R0, doom_memory_copy_tail

doom_memory_copy_four:
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    ADDI T4, T4, -1
    BNE T4, R0, doom_memory_copy_four
doom_memory_copy_tail:
    BEQ T5, R0, doom_memory_copy_end
doom_memory_copy_one:
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    ADDI T5, T5, -1
    BNE T5, R0, doom_memory_copy_one
doom_memory_copy_end:
    BNE T0, R0, doom_memory_copy_continue
    BEQ T1, R0, doom_memory_return
doom_memory_copy_continue:
    BNE A0, T7, doom_memory_copy_dst_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_memory_copy_dst_next:
    BNE A2, T7, doom_memory_copy_src_next
    ADD A2, R0, R0
    ADDI A3, A3, 1
doom_memory_copy_src_next:
    BEQ R0, R0, doom_memory_copy_chunk

; A0/A1 destination, A2 value, SP-1 count low, SP-2 count high.
Doom_Memset:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    BNE T0, R0, doom_set_nonempty
    BEQ T1, R0, doom_memory_return
doom_set_nonempty:
    LI T7, 64000
    LI T3, 65535
    BNE A1, T3, doom_set_dst_stack
    ADD A1, GP, R0
doom_set_dst_stack:
    LI T3, 65534
    BNE A1, T3, doom_set_dst_ready
    ADD A1, SB, R0
doom_set_dst_ready:
doom_memory_set_chunk:
    SUB T2, T7, A0
    BNE T1, R0, doom_memory_set_count_chunk
    SLTU SC, T0, T2
    BEQ SC, R0, doom_memory_set_count_chunk
    ADD T2, T0, R0
doom_memory_set_count_chunk:
    SLTU SC, T0, T2
    SUB T0, T0, T2
    SUB T1, T1, SC
    SRLI T4, T2, 2
    ADD T5, T4, T4
    ADD T5, T5, T5
    SUB T5, T2, T5
    BEQ T4, R0, doom_memory_set_tail
doom_memory_set_four:
    STORE A2, A1, A0
    ADDI A0, A0, 1
    STORE A2, A1, A0
    ADDI A0, A0, 1
    STORE A2, A1, A0
    ADDI A0, A0, 1
    STORE A2, A1, A0
    ADDI A0, A0, 1
    ADDI T4, T4, -1
    BNE T4, R0, doom_memory_set_four
doom_memory_set_tail:
    BEQ T5, R0, doom_memory_set_end
doom_memory_set_one:
    STORE A2, A1, A0
    ADDI A0, A0, 1
    ADDI T5, T5, -1
    BNE T5, R0, doom_memory_set_one
doom_memory_set_end:
    BNE T0, R0, doom_memory_set_continue
    BEQ T1, R0, doom_memory_return
doom_memory_set_continue:
    ADD A0, R0, R0
    ADDI A1, A1, 1
    BEQ R0, R0, doom_memory_set_chunk

; Copy backwards whenever destination is above source; this is safe for both
; overlapping and disjoint ranges and avoids computing source+size to decide.
Doom_Memmove:
    ADD RV0, A0, R0
    ADD RV1, A1, R0
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    BNE T0, R0, doom_move_nonempty
    BEQ T1, R0, doom_memory_return
doom_move_nonempty:
    LI T7, 64000
    LI T3, 65535
    BNE A1, T3, doom_move_dst_stack
    ADD A1, GP, R0
doom_move_dst_stack:
    LI T3, 65534
    BNE A1, T3, doom_move_dst_ready
    ADD A1, SB, R0
doom_move_dst_ready:
    LI T3, 65535
    BNE A3, T3, doom_move_src_stack
    ADD A3, GP, R0
doom_move_src_stack:
    LI T3, 65534
    BNE A3, T3, doom_move_src_ready
    ADD A3, SB, R0
doom_move_src_ready:
    SLTU T2, A1, A3
    BNE T2, R0, doom_memory_copy_chunk
    SLTU T2, A3, A1
    BNE T2, R0, doom_memory_move_endpoints
    SLTU T2, A0, A2
    BNE T2, R0, doom_memory_copy_chunk
    BEQ A0, A2, doom_memory_return

doom_memory_move_endpoints:
    ; Advance each cursor by the full 32-bit count, preserving that count.
    ; Work scales with the number of banks, never with the number of cells.
    ADD T2, T0, R0
    ADD T3, T1, R0
doom_memory_move_dst_bank:
    SUB T4, T7, A0
    BNE T3, R0, doom_memory_move_dst_advance
    SLTU T5, T2, T4
    BNE T5, R0, doom_memory_move_dst_tail
doom_memory_move_dst_advance:
    SLTU T5, T2, T4
    SUB T2, T2, T4
    SUB T3, T3, T5
    ADD A0, R0, R0
    ADDI A1, A1, 1
    BEQ R0, R0, doom_memory_move_dst_bank
doom_memory_move_dst_tail:
    ADD A0, A0, T2
    ADD T2, T0, R0
    ADD T3, T1, R0
doom_memory_move_src_bank:
    SUB T4, T7, A2
    BNE T3, R0, doom_memory_move_src_advance
    SLTU T5, T2, T4
    BNE T5, R0, doom_memory_move_src_tail
doom_memory_move_src_advance:
    SLTU T5, T2, T4
    SUB T2, T2, T4
    SUB T3, T3, T5
    ADD A2, R0, R0
    ADDI A3, A3, 1
    BEQ R0, R0, doom_memory_move_src_bank
doom_memory_move_src_tail:
    ADD A2, A2, T2
doom_memory_move_back:
    BNE A0, R0, doom_memory_move_dst_previous
    ADD A0, T7, R0
    ADDI A1, A1, -1
doom_memory_move_dst_previous:
    ADDI A0, A0, -1
    BNE A2, R0, doom_memory_move_src_previous
    ADD A2, T7, R0
    ADDI A3, A3, -1
doom_memory_move_src_previous:
    ADDI A2, A2, -1
    LOAD T6, A3, A2
    STORE T6, A1, A0
    ADD T2, T0, R0
    ADDI T0, T0, -1
    SLTU SC, T2, T0
    SUB T1, T1, SC
    BNE T0, R0, doom_memory_move_back
    BNE T1, R0, doom_memory_move_back
doom_memory_return:
    JALR R0, RAB, RA
