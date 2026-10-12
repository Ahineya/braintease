; Doom rendering kernels for BANK_SIZE=64000 and 16-bit C bytes.
; Leaf ABI: preserve SP/FP/SB/GP, S0-S3 and RA/RAB. All work uses caller-saved
; registers. No VM extension or host rendering shortcut is involved.
;
; void Doom_ConvertRGB565(u16 *dst, const byte *src, const u16 *palette,
;                        unsigned count)
; A0/A1 = destination address/bank; A2/A3 = source address/bank.
; Stack SP-1 = palette address, SP-2 = palette bank, SP-3 = count.
; Source and destination are non-overlapping, normalized C pointers. Count
; may be zero. Palette contains 256 entries and may straddle a bank.
Doom_ConvertRGB565:
    ADDI SC, SP, -3
    LOAD T2, SB, SC
    BEQ T2, R0, doom_rgb_done
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC
    LI T3, 64000
    LI T5, 65535
    BNE A1, T5, doom_rgb_dst_stack
    ADD A1, GP, R0
doom_rgb_dst_stack:
    LI T5, 65534
    BNE A1, T5, doom_rgb_dst_resolved
    ADD A1, SB, R0
doom_rgb_dst_resolved:
    LI T5, 65535
    BNE A3, T5, doom_rgb_src_stack
    ADD A3, GP, R0
doom_rgb_src_stack:
    LI T5, 65534
    BNE A3, T5, doom_rgb_src_resolved
    ADD A3, SB, R0
doom_rgb_src_resolved:
    LI T5, 65535
    BNE T1, T5, doom_rgb_palette_stack
    ADD T1, GP, R0
doom_rgb_palette_stack:
    LI T5, 65534
    BNE T1, T5, doom_rgb_palette_resolved
    ADD T1, SB, R0
doom_rgb_palette_resolved:
    ; A palette wholly inside one bank needs no per-pixel bank arithmetic.
    LI T5, 63744
    SLTU T5, T5, T0
    BNE T5, R0, doom_rgb_split_palette

doom_rgb_chunk:
    ; Clip the chunk to both bank ends; then normalize only once per chunk.
    ADD T4, T2, R0
    SUB T5, T3, A0
    SLTU RV0, T5, T4
    BEQ RV0, R0, doom_rgb_dst_chunk
    ADD T4, T5, R0
doom_rgb_dst_chunk:
    SUB T5, T3, A2
    SLTU RV0, T5, T4
    BEQ RV0, R0, doom_rgb_src_chunk
    ADD T4, T5, R0
doom_rgb_src_chunk:
    SUB T2, T2, T4
    SRLI T5, T4, 2
    ANDI T4, T4, 3
    BEQ T5, R0, doom_rgb_tail

doom_rgb_four:
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    LOAD T6, T1, T7
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    LOAD T6, T1, T7
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    LOAD T6, T1, T7
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    LOAD T6, T1, T7
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    ADDI T5, T5, -1
    BNE T5, R0, doom_rgb_four

doom_rgb_tail:
    BEQ T4, R0, doom_rgb_chunk_end
doom_rgb_one:
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    LOAD T6, T1, T7
    STORE T6, A1, A0
    ADDI A2, A2, 1
    ADDI A0, A0, 1
    ADDI T4, T4, -1
    BNE T4, R0, doom_rgb_one

doom_rgb_chunk_end:
    BEQ T2, R0, doom_rgb_done
    BNE A0, T3, doom_rgb_dst_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_rgb_dst_next:
    BNE A2, T3, doom_rgb_src_next
    ADD A2, R0, R0
    ADDI A3, A3, 1
doom_rgb_src_next:
    BEQ R0, R0, doom_rgb_chunk

    ; Uncommon palette straddling a bank: resolve the selected entry without
    ; altering the palette base. 63999+255 fits a native word at bank size 64000.
doom_rgb_split_palette:
    LOAD T6, A3, A2
    ANDI T6, T6, 255
    ADD T7, T0, T6
    ADD RV1, T1, R0
    SLTU T5, T7, T3
    BNE T5, R0, doom_rgb_palette_entry
    SUB T7, T7, T3
    ADDI RV1, RV1, 1
doom_rgb_palette_entry:
    LOAD T6, RV1, T7
    STORE T6, A1, A0
    ADDI T2, T2, -1
    BEQ T2, R0, doom_rgb_done
    ADDI A0, A0, 1
    BNE A0, T3, doom_rgb_split_dst
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_rgb_split_dst:
    ADDI A2, A2, 1
    BNE A2, T3, doom_rgb_split_src
    ADD A2, R0, R0
    ADDI A3, A3, 1
doom_rgb_split_src:
    BEQ R0, R0, doom_rgb_split_palette

doom_rgb_done:
    JALR R0, RAB, RA

; void Doom_CopyPatchColumn(byte *dst, const byte *src, unsigned count)
; A0/A1 = destination; A2/A3 = source; stack SP-1 = count.
; Copy a Doom post down a 320-cell-wide framebuffer. Zero-length posts are
; valid. The source and destination can each cross any number of banks.
Doom_CopyPatchColumn:
    ADDI SC, SP, -1
    LOAD T2, SB, SC
    BEQ T2, R0, doom_patch_done
    LI T3, 64000
    LI T5, 65535
    BNE A1, T5, doom_patch_dst_stack
    ADD A1, GP, R0
doom_patch_dst_stack:
    LI T5, 65534
    BNE A1, T5, doom_patch_dst_resolved
    ADD A1, SB, R0
doom_patch_dst_resolved:
    LI T5, 65535
    BNE A3, T5, doom_patch_src_stack
    ADD A3, GP, R0
doom_patch_src_stack:
    LI T5, 65534
    BNE A3, T5, doom_patch_src_resolved
    ADD A3, SB, R0
doom_patch_src_resolved:
doom_patch_pixel:
    LOAD T0, A3, A2
    STORE T0, A1, A0
    ADDI T2, T2, -1
    BEQ T2, R0, doom_patch_done
    ADDI A2, A2, 1
    BNE A2, T3, doom_patch_src_next
    ADD A2, R0, R0
    ADDI A3, A3, 1
doom_patch_src_next:
    ADDI A0, A0, 320
    SLTU T1, A0, T3
    BNE T1, R0, doom_patch_pixel
    SUB A0, A0, T3
    ADDI A1, A1, 1
    BEQ R0, R0, doom_patch_pixel
doom_patch_done:
    JALR R0, RAB, RA
