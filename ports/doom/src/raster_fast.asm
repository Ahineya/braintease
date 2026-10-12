; Doom's high-detail column/span inner loops on the existing Ripple ISA.
; Bank size is 64000 cells (the port's fixed ABI); byte pixels occupy one cell.
; Args: A0/A1 destination, A2/A3 source. Remaining words below entry SP:
; -1/-2 colormap offset/bank, -3/-4 position lo/hi, -5/-6 step lo/hi,
; -7 pixel count. Column samples (position >> 16) & 127 with stride 320.
; Span samples ((position >> 4) & 4032) | (position >> 26) with stride 1.
; Packed span addition intentionally includes carry between the two halves,
; exactly as the upstream 32-bit unsigned expression does.
; Source/colormap entries are decoded octets (0..255). Pointers are canonical.
; Zero count performs no memory accesses through any supplied pointer.
; Clobbers A0-A3,T0-T7,RV0,RV1,SC; preserves all callee-saved registers.
; Common path has whole source and colormap tables in one bank. The fallback
; handles split tables, including 16-bit offset overflow for 4096-cell flats.
.code

Doom_DrawColumn:
    ADDI SC, SP, -7
    LOAD T6, SB, SC
    BEQ T6, R0, L_doom_raster_column_return
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    ADDI SC, SP, -3
    LOAD T2, SB, SC
    ADDI SC, SP, -4
    LOAD T3, SB, SC
    ADDI SC, SP, -5
    LOAD T4, SB, SC
    ADDI SC, SP, -6
    LOAD T5, SB, SC
    LI SC, -1
    BNE A1, SC, L_doom_raster_column_A1_stack
    ADD A1, GP, R0
    BEQ R0, R0, L_doom_raster_column_A1_ready
L_doom_raster_column_A1_stack:
    LI SC, -2
    BNE A1, SC, L_doom_raster_column_A1_ready
    ADD A1, SB, R0
L_doom_raster_column_A1_ready:
    LI SC, -1
    BNE A3, SC, L_doom_raster_column_A3_stack
    ADD A3, GP, R0
    BEQ R0, R0, L_doom_raster_column_A3_ready
L_doom_raster_column_A3_stack:
    LI SC, -2
    BNE A3, SC, L_doom_raster_column_A3_ready
    ADD A3, SB, R0
L_doom_raster_column_A3_ready:
    LI SC, -1
    BNE T1, SC, L_doom_raster_column_T1_stack
    ADD T1, GP, R0
    BEQ R0, R0, L_doom_raster_column_T1_ready
L_doom_raster_column_T1_stack:
    LI SC, -2
    BNE T1, SC, L_doom_raster_column_T1_ready
    ADD T1, SB, R0
L_doom_raster_column_T1_ready:
    LI T7, 64000
    LI SC, 63873
    SLTU SC, A2, SC
    BEQ SC, R0, L_doom_raster_column_split
    LI SC, 63745
    SLTU SC, T0, SC
    BEQ SC, R0, L_doom_raster_column_split
L_doom_raster_column_fast:
    ANDI RV0, T3, 127
    ADD RV0, A2, RV0
    LOAD RV0, A3, RV0
    ADD RV0, T0, RV0
    LOAD RV0, T1, RV0
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 320
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_fast_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_fast_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_column_fast
    JALR R0, RAB, RA
L_doom_raster_column_split:
    ANDI RV0, T3, 127
    ADD RV0, A2, RV0
    SLTU SC, RV0, A2
    BNE SC, R0, L_doom_raster_column_source_cross
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_column_source_here
L_doom_raster_column_source_cross:
    ADDI RV0, RV0, 1536
    ADDI SC, A3, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_column_source_loaded
L_doom_raster_column_source_here:
    LOAD RV0, A3, RV0
L_doom_raster_column_source_loaded:
    ADD RV0, T0, RV0
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_column_map_here
    SUB RV0, RV0, T7
    ADDI SC, T1, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_column_map_loaded
L_doom_raster_column_map_here:
    LOAD RV0, T1, RV0
L_doom_raster_column_map_loaded:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 320
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_split_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_split_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_column_split
    JALR R0, RAB, RA
L_doom_raster_column_return:
    JALR R0, RAB, RA

Doom_DrawSpan:
    ADDI SC, SP, -7
    LOAD T6, SB, SC
    BEQ T6, R0, L_doom_raster_span_return
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    ADDI SC, SP, -3
    LOAD T2, SB, SC
    ADDI SC, SP, -4
    LOAD T3, SB, SC
    ADDI SC, SP, -5
    LOAD T4, SB, SC
    ADDI SC, SP, -6
    LOAD T5, SB, SC
    LI SC, -1
    BNE A1, SC, L_doom_raster_span_A1_stack
    ADD A1, GP, R0
    BEQ R0, R0, L_doom_raster_span_A1_ready
L_doom_raster_span_A1_stack:
    LI SC, -2
    BNE A1, SC, L_doom_raster_span_A1_ready
    ADD A1, SB, R0
L_doom_raster_span_A1_ready:
    LI SC, -1
    BNE A3, SC, L_doom_raster_span_A3_stack
    ADD A3, GP, R0
    BEQ R0, R0, L_doom_raster_span_A3_ready
L_doom_raster_span_A3_stack:
    LI SC, -2
    BNE A3, SC, L_doom_raster_span_A3_ready
    ADD A3, SB, R0
L_doom_raster_span_A3_ready:
    LI SC, -1
    BNE T1, SC, L_doom_raster_span_T1_stack
    ADD T1, GP, R0
    BEQ R0, R0, L_doom_raster_span_T1_ready
L_doom_raster_span_T1_stack:
    LI SC, -2
    BNE T1, SC, L_doom_raster_span_T1_ready
    ADD T1, SB, R0
L_doom_raster_span_T1_ready:
    LI T7, 64000
    LI SC, 59905
    SLTU SC, A2, SC
    BEQ SC, R0, L_doom_raster_span_split
    LI SC, 63745
    SLTU SC, T0, SC
    BEQ SC, R0, L_doom_raster_span_split
L_doom_raster_span_fast:
    SRLI RV0, T2, 4
    ANDI RV0, RV0, 4032
    SRLI SC, T3, 10
    OR RV0, RV0, SC
    ADD RV0, A2, RV0
    LOAD RV0, A3, RV0
    ADD RV0, T0, RV0
    LOAD RV0, T1, RV0
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_fast_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_fast_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_span_fast
    JALR R0, RAB, RA
L_doom_raster_span_split:
    SRLI RV0, T2, 4
    ANDI RV0, RV0, 4032
    SRLI SC, T3, 10
    OR RV0, RV0, SC
    ADD RV0, A2, RV0
    SLTU SC, RV0, A2
    BNE SC, R0, L_doom_raster_span_source_cross
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_span_source_here
L_doom_raster_span_source_cross:
    ADDI RV0, RV0, 1536
    ADDI SC, A3, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_span_source_loaded
L_doom_raster_span_source_here:
    LOAD RV0, A3, RV0
L_doom_raster_span_source_loaded:
    ADD RV0, T0, RV0
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_span_map_here
    SUB RV0, RV0, T7
    ADDI SC, T1, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_span_map_loaded
L_doom_raster_span_map_here:
    LOAD RV0, T1, RV0
L_doom_raster_span_map_loaded:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_split_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_split_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_span_split
    JALR R0, RAB, RA
L_doom_raster_span_return:
    JALR R0, RAB, RA

; Low-detail variants duplicate each sample horizontally. Sampling and 32-bit
; carry rules match the high-detail leaves; only destination stepping differs.
Doom_DrawColumnLow:
    ADDI SC, SP, -7
    LOAD T6, SB, SC
    BEQ T6, R0, L_doom_raster_column_low_return
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    ADDI SC, SP, -3
    LOAD T2, SB, SC
    ADDI SC, SP, -4
    LOAD T3, SB, SC
    ADDI SC, SP, -5
    LOAD T4, SB, SC
    ADDI SC, SP, -6
    LOAD T5, SB, SC
    LI SC, -1
    BNE A1, SC, L_doom_raster_column_low_A1_stack
    ADD A1, GP, R0
    BEQ R0, R0, L_doom_raster_column_low_A1_ready
L_doom_raster_column_low_A1_stack:
    LI SC, -2
    BNE A1, SC, L_doom_raster_column_low_A1_ready
    ADD A1, SB, R0
L_doom_raster_column_low_A1_ready:
    LI SC, -1
    BNE A3, SC, L_doom_raster_column_low_A3_stack
    ADD A3, GP, R0
    BEQ R0, R0, L_doom_raster_column_low_A3_ready
L_doom_raster_column_low_A3_stack:
    LI SC, -2
    BNE A3, SC, L_doom_raster_column_low_A3_ready
    ADD A3, SB, R0
L_doom_raster_column_low_A3_ready:
    LI SC, -1
    BNE T1, SC, L_doom_raster_column_low_T1_stack
    ADD T1, GP, R0
    BEQ R0, R0, L_doom_raster_column_low_T1_ready
L_doom_raster_column_low_T1_stack:
    LI SC, -2
    BNE T1, SC, L_doom_raster_column_low_T1_ready
    ADD T1, SB, R0
L_doom_raster_column_low_T1_ready:
    LI T7, 64000
    LI SC, 63873
    SLTU SC, A2, SC
    BEQ SC, R0, L_doom_raster_column_low_split
    LI SC, 63745
    SLTU SC, T0, SC
    BEQ SC, R0, L_doom_raster_column_low_split
L_doom_raster_column_low_fast:
    ANDI RV0, T3, 127
    ADD RV0, A2, RV0
    LOAD RV0, A3, RV0
    ADD RV0, T0, RV0
    LOAD RV0, T1, RV0
    STORE RV0, A1, A0
    ; Each low-detail sample occupies adjacent cells, including when the
    ; pair straddles a bank. A0 then points at the second cell of the pair.
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_low_fast_pair
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_low_fast_pair:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 319
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_low_fast_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_low_fast_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_column_low_fast
    JALR R0, RAB, RA
L_doom_raster_column_low_split:
    ANDI RV0, T3, 127
    ADD RV0, A2, RV0
    SLTU SC, RV0, A2
    BNE SC, R0, L_doom_raster_column_low_source_cross
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_column_low_source_here
L_doom_raster_column_low_source_cross:
    ADDI RV0, RV0, 1536
    ADDI SC, A3, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_column_low_source_loaded
L_doom_raster_column_low_source_here:
    LOAD RV0, A3, RV0
L_doom_raster_column_low_source_loaded:
    ADD RV0, T0, RV0
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_column_low_map_here
    SUB RV0, RV0, T7
    ADDI SC, T1, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_column_low_map_loaded
L_doom_raster_column_low_map_here:
    LOAD RV0, T1, RV0
L_doom_raster_column_low_map_loaded:
    STORE RV0, A1, A0
    ; Each low-detail sample occupies adjacent cells, including when the
    ; pair straddles a bank. A0 then points at the second cell of the pair.
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_low_split_pair
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_low_split_pair:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 319
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_column_low_split_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_column_low_split_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_column_low_split
    JALR R0, RAB, RA
L_doom_raster_column_low_return:
    JALR R0, RAB, RA

Doom_DrawSpanLow:
    ADDI SC, SP, -7
    LOAD T6, SB, SC
    BEQ T6, R0, L_doom_raster_span_low_return
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    ADDI SC, SP, -3
    LOAD T2, SB, SC
    ADDI SC, SP, -4
    LOAD T3, SB, SC
    ADDI SC, SP, -5
    LOAD T4, SB, SC
    ADDI SC, SP, -6
    LOAD T5, SB, SC
    LI SC, -1
    BNE A1, SC, L_doom_raster_span_low_A1_stack
    ADD A1, GP, R0
    BEQ R0, R0, L_doom_raster_span_low_A1_ready
L_doom_raster_span_low_A1_stack:
    LI SC, -2
    BNE A1, SC, L_doom_raster_span_low_A1_ready
    ADD A1, SB, R0
L_doom_raster_span_low_A1_ready:
    LI SC, -1
    BNE A3, SC, L_doom_raster_span_low_A3_stack
    ADD A3, GP, R0
    BEQ R0, R0, L_doom_raster_span_low_A3_ready
L_doom_raster_span_low_A3_stack:
    LI SC, -2
    BNE A3, SC, L_doom_raster_span_low_A3_ready
    ADD A3, SB, R0
L_doom_raster_span_low_A3_ready:
    LI SC, -1
    BNE T1, SC, L_doom_raster_span_low_T1_stack
    ADD T1, GP, R0
    BEQ R0, R0, L_doom_raster_span_low_T1_ready
L_doom_raster_span_low_T1_stack:
    LI SC, -2
    BNE T1, SC, L_doom_raster_span_low_T1_ready
    ADD T1, SB, R0
L_doom_raster_span_low_T1_ready:
    LI T7, 64000
    LI SC, 59905
    SLTU SC, A2, SC
    BEQ SC, R0, L_doom_raster_span_low_split
    LI SC, 63745
    SLTU SC, T0, SC
    BEQ SC, R0, L_doom_raster_span_low_split
L_doom_raster_span_low_fast:
    SRLI RV0, T2, 4
    ANDI RV0, RV0, 4032
    SRLI SC, T3, 10
    OR RV0, RV0, SC
    ADD RV0, A2, RV0
    LOAD RV0, A3, RV0
    ADD RV0, T0, RV0
    LOAD RV0, T1, RV0
    STORE RV0, A1, A0
    ; Each low-detail sample occupies adjacent cells, including when the
    ; pair straddles a bank. A0 then points at the second cell of the pair.
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_low_fast_pair
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_low_fast_pair:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_low_fast_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_low_fast_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_span_low_fast
    JALR R0, RAB, RA
L_doom_raster_span_low_split:
    SRLI RV0, T2, 4
    ANDI RV0, RV0, 4032
    SRLI SC, T3, 10
    OR RV0, RV0, SC
    ADD RV0, A2, RV0
    SLTU SC, RV0, A2
    BNE SC, R0, L_doom_raster_span_low_source_cross
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_span_low_source_here
L_doom_raster_span_low_source_cross:
    ADDI RV0, RV0, 1536
    ADDI SC, A3, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_span_low_source_loaded
L_doom_raster_span_low_source_here:
    LOAD RV0, A3, RV0
L_doom_raster_span_low_source_loaded:
    ADD RV0, T0, RV0
    SLTU SC, RV0, T7
    BNE SC, R0, L_doom_raster_span_low_map_here
    SUB RV0, RV0, T7
    ADDI SC, T1, 1
    LOAD RV0, SC, RV0
    BEQ R0, R0, L_doom_raster_span_low_map_loaded
L_doom_raster_span_low_map_here:
    LOAD RV0, T1, RV0
L_doom_raster_span_low_map_loaded:
    STORE RV0, A1, A0
    ; Each low-detail sample occupies adjacent cells, including when the
    ; pair straddles a bank. A0 then points at the second cell of the pair.
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_low_split_pair
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_low_split_pair:
    STORE RV0, A1, A0
    ADD T2, T2, T4
    SLTU RV0, T2, T4
    ADD T3, T3, T5
    ADD T3, T3, RV0
    ADDI A0, A0, 1
    SLTU SC, A0, T7
    BNE SC, R0, L_doom_raster_span_low_split_next
    SUB A0, A0, T7
    ADDI A1, A1, 1
L_doom_raster_span_low_split_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_raster_span_low_split
    JALR R0, RAB, RA
L_doom_raster_span_low_return:
    JALR R0, RAB, RA
