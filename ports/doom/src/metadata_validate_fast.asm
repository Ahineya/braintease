; Port-local cooked directory validation for BANK_SIZE=64000.
; Validate packed target-ABI lumpinfo records without interpreting pointer
; placeholders as C pointers. This is a guest leaf using the existing ISA.
;
; C declaration:
; uint32_t Doom_ValidateMetadata(const void *records,
;     unsigned int count, unsigned int first_index, uint32_t file_length);
;
; A0/A1 = canonical or global/stack-tagged records pointer.
; A2 = record count (0..16), A3 = first global lump index.
; SP-1 = file length low, SP-2 = file length high.
; first_index + count must be <=32767; invalid spans return 4.
;
; RV0: 0 valid, 1 invalid name cell, 2 invalid placeholder/link,
;      3 invalid file range, 4 invalid count/index span, 5 invalid pointer.
; Count zero succeeds without reading either records or stack arguments.
; Physical offset 0 in bank 0 is distinct from the C null tag 65533.
; RV1 reports the bad record index (first_index on an argument error),
; and zero on success. The combined C result is code | (index << 16).
;
; Preserves S0-S3, SB, SP, FP, GP, RA/RAB. Clobbers arguments, temporaries,
; SC and X0-X3 (caller scratch). Never modifies input or guest memory.
; 64000-cell bank crossing is handled after every read, including a split
; u32 or placeholder. All checks use compare/subtract, with no bitwise ops.
.code
Doom_ValidateMetadata:
    BEQ A2, R0, doom_asset_validate_success
    LI T2, 16
    SLTU SC, T2, A2
    BNE SC, R0, doom_asset_validate_bad_count
    ADD T2, A3, A2
    SLTU SC, T2, A3
    BNE SC, R0, doom_asset_validate_bad_count
    LI T3, 32767
    SLTU SC, T3, T2
    BNE SC, R0, doom_asset_validate_bad_count

    LI X1, 64000
    SLTU SC, A0, X1
    BEQ SC, R0, doom_asset_validate_bad_pointer
    LI T2, 65535
    BNE A1, T2, doom_asset_validate_stack_tag
    ADD A1, GP, R0
doom_asset_validate_stack_tag:
    LI T2, 65534
    BNE A1, T2, doom_asset_validate_null_tag
    ADD A1, SB, R0
doom_asset_validate_null_tag:
    LI T2, 65533
    BEQ A1, T2, doom_asset_validate_bad_pointer
    ADDI SC, SP, -1
    LOAD T0, SB, SC
    ADDI SC, SP, -2
    LOAD T1, SB, SC

doom_asset_validate_record:
    ; Cells 0..7: unsigned source octets, including padding after a NUL.
    LI T2, 8
    LI X2, 255
doom_asset_validate_name:
    LOAD T3, A1, A0
    SLTU SC, X2, T3
    BNE SC, R0, doom_asset_validate_bad_name
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_name_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_name_next:
    ADDI T2, T2, -1
    BNE T2, R0, doom_asset_validate_name

    ; Cells 8..9: WAD-file pointer placeholder, both words must be zero.
    LI T2, 2
doom_asset_validate_file_pointer:
    LOAD T3, A1, A0
    BNE T3, R0, doom_asset_validate_bad_link
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_file_pointer_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_file_pointer_next:
    ADDI T2, T2, -1
    BNE T2, R0, doom_asset_validate_file_pointer

    ; Cells 10..13: 32-bit position and size, low word first.
    LOAD T4, A1, A0
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_position_high
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_position_high:
    LOAD T5, A1, A0
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_size_low
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_size_low:
    LOAD T6, A1, A0
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_size_high
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_size_high:
    LOAD T7, A1, A0
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_range
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_range:
    ; Require position <= file_length using an unsigned high/low comparison.
    SLTU SC, T1, T5
    BNE SC, R0, doom_asset_validate_bad_range
    SLTU SC, T5, T1
    BNE SC, R0, doom_asset_validate_remaining
    SLTU SC, T0, T4
    BNE SC, R0, doom_asset_validate_bad_range
doom_asset_validate_remaining:
    ; Remaining = file_length - position. Borrow is computed before SUB.
    SLTU SC, T0, T4
    SUB X0, T0, T4
    SUB X3, T1, T5
    SUB X3, X3, SC
    ; Require size <= remaining, avoiding overflow from position + size.
    SLTU SC, X3, T7
    BNE SC, R0, doom_asset_validate_bad_range
    SLTU SC, T7, X3
    BNE SC, R0, doom_asset_validate_cache
    SLTU SC, X0, T6
    BNE SC, R0, doom_asset_validate_bad_range

doom_asset_validate_cache:
    ; Cells 14..15: cache placeholder. The C relocation must assign canonical
    ; NULL afterwards; its bank tag differs from this serialized zero pair.
    LI T2, 2
doom_asset_validate_cache_pointer:
    LOAD T3, A1, A0
    BNE T3, R0, doom_asset_validate_bad_link
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_cache_pointer_next
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_cache_pointer_next:
    ADDI T2, T2, -1
    BNE T2, R0, doom_asset_validate_cache_pointer

    ; Cell 16: missing link (65535), or a strictly earlier global record.
    LOAD T3, A1, A0
    LI X2, 65535
    BEQ T3, X2, doom_asset_validate_next_low_done
    SLTU SC, T3, A3
    BEQ SC, R0, doom_asset_validate_bad_link
doom_asset_validate_next_low_done:
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_next_high
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_next_high:
    ; Cell 17: next-index high word is always zero, even for the sentinel.
    LOAD T3, A1, A0
    BNE T3, R0, doom_asset_validate_bad_link
    ADDI A0, A0, 1
    BNE A0, X1, doom_asset_validate_record_done
    ADD A0, R0, R0
    ADDI A1, A1, 1
doom_asset_validate_record_done:
    ADDI A3, A3, 1
    ADDI A2, A2, -1
    BNE A2, R0, doom_asset_validate_record
doom_asset_validate_success:
    ADD RV0, R0, R0
    ADD RV1, R0, R0
    JALR R0, RAB, RA

doom_asset_validate_bad_name:
    LI RV0, 1
    BEQ R0, R0, doom_asset_validate_return
doom_asset_validate_bad_link:
    LI RV0, 2
    BEQ R0, R0, doom_asset_validate_return
doom_asset_validate_bad_range:
    LI RV0, 3
    BEQ R0, R0, doom_asset_validate_return
doom_asset_validate_bad_count:
    LI RV0, 4
    BEQ R0, R0, doom_asset_validate_return
doom_asset_validate_bad_pointer:
    LI RV0, 5
doom_asset_validate_return:
    ADD RV1, A3, R0
    JALR R0, RAB, RA
