; Doom 16.16 arithmetic using only existing 16-bit Ripple instructions.
; A0/A1=a low/high, A2/A3=b low/high; RV0/RV1=result low/high.
; Computes bits16..47 of the signed64 product. No 64-bit result is needed.
; high16(x*y) is reconstructed exactly with four 8x8 native multiplications.
; For general inputs the unsigned48 product is corrected by subtracting
; b.lo from result.hi when a<0, and a.lo when b<0. This implements two's
; complement signed extension, including INT32_MIN and overflowing results.
; All callee-saved registers, SP, FP, RA/RAB, GP and SB are untouched.
.code
FixedMul:
    BEQ A0, R0, L_doom_fixed_a_integer
    BEQ A2, R0, L_doom_fixed_b_integer
    ANDI T5, A0, 255
    SRLI T6, A0, 8
    ANDI T7, A2, 255
    SRLI SC, A2, 8
    MUL T0, T6, SC
    MUL RV0, T5, T7
    SRLI RV0, RV0, 8
    MUL T5, T5, SC
    MUL T6, T6, T7
    ANDI T7, T5, 255
    ADD RV0, RV0, T7
    ANDI T7, T6, 255
    ADD RV0, RV0, T7
    SRLI RV0, RV0, 8
    SRLI T5, T5, 8
    SRLI T6, T6, 8
    ADD T0, T0, T5
    ADD T0, T0, T6
    ADD T0, T0, RV0
    ANDI T5, A0, 255
    SRLI T6, A0, 8
    ANDI T7, A3, 255
    SRLI SC, A3, 8
    MUL T2, T6, SC
    MUL RV0, T5, T7
    SRLI RV0, RV0, 8
    MUL T5, T5, SC
    MUL T6, T6, T7
    ANDI T7, T5, 255
    ADD RV0, RV0, T7
    ANDI T7, T6, 255
    ADD RV0, RV0, T7
    SRLI RV0, RV0, 8
    SRLI T5, T5, 8
    SRLI T6, T6, 8
    ADD T2, T2, T5
    ADD T2, T2, T6
    ADD T2, T2, RV0
    ANDI T5, A1, 255
    SRLI T6, A1, 8
    ANDI T7, A2, 255
    SRLI SC, A2, 8
    MUL T4, T6, SC
    MUL RV0, T5, T7
    SRLI RV0, RV0, 8
    MUL T5, T5, SC
    MUL T6, T6, T7
    ANDI T7, T5, 255
    ADD RV0, RV0, T7
    ANDI T7, T6, 255
    ADD RV0, RV0, T7
    SRLI RV0, RV0, 8
    SRLI T5, T5, 8
    SRLI T6, T6, 8
    ADD T4, T4, T5
    ADD T4, T4, T6
    ADD T4, T4, RV0
    MUL T1, A0, A3
    MUL T3, A1, A2
    MUL RV1, A1, A3
    ADD RV1, RV1, T2
    ADD RV1, RV1, T4
    ADD RV0, T0, T1
    SLTU SC, RV0, T1
    ADD RV1, RV1, SC
    ADD RV0, RV0, T3
    SLTU SC, RV0, T3
    ADD RV1, RV1, SC
    SLT SC, A1, R0
    BEQ SC, R0, L_doom_fixed_a_positive
    SUB RV1, RV1, A2
L_doom_fixed_a_positive:
    SLT SC, A3, R0
    BEQ SC, R0, L_doom_fixed_return
    SUB RV1, RV1, A0
L_doom_fixed_return:
    JALR R0, RAB, RA

L_doom_fixed_a_integer:
    BEQ A2, R0, L_doom_fixed_both_integer
    ANDI T5, A1, 255
    SRLI T6, A1, 8
    ANDI T7, A2, 255
    SRLI SC, A2, 8
    MUL T0, T6, SC
    MUL RV0, T5, T7
    SRLI RV0, RV0, 8
    MUL T5, T5, SC
    MUL T6, T6, T7
    ANDI T7, T5, 255
    ADD RV0, RV0, T7
    ANDI T7, T6, 255
    ADD RV0, RV0, T7
    SRLI RV0, RV0, 8
    SRLI T5, T5, 8
    SRLI T6, T6, 8
    ADD T0, T0, T5
    ADD T0, T0, T6
    ADD T0, T0, RV0
    MUL RV0, A1, A2
    MUL RV1, A1, A3
    ADD RV1, RV1, T0
    SLT SC, A1, R0
    BEQ SC, R0, L_doom_fixed_return
    SUB RV1, RV1, A2
    JALR R0, RAB, RA

L_doom_fixed_b_integer:
    ANDI T5, A0, 255
    SRLI T6, A0, 8
    ANDI T7, A3, 255
    SRLI SC, A3, 8
    MUL T0, T6, SC
    MUL RV0, T5, T7
    SRLI RV0, RV0, 8
    MUL T5, T5, SC
    MUL T6, T6, T7
    ANDI T7, T5, 255
    ADD RV0, RV0, T7
    ANDI T7, T6, 255
    ADD RV0, RV0, T7
    SRLI RV0, RV0, 8
    SRLI T5, T5, 8
    SRLI T6, T6, 8
    ADD T0, T0, T5
    ADD T0, T0, T6
    ADD T0, T0, RV0
    MUL RV0, A0, A3
    MUL RV1, A1, A3
    ADD RV1, RV1, T0
    SLT SC, A3, R0
    BEQ SC, R0, L_doom_fixed_return
    SUB RV1, RV1, A0
    JALR R0, RAB, RA

L_doom_fixed_both_integer:
    LI RV0, 0
    MUL RV1, A1, A3
    JALR R0, RAB, RA

; Signed 16.16 division with Doom's conservative saturation guard.
; The guard makes the unsigned quotient fit in 30 bits. Seed the remainder
; with abs(a)>>14, consume the remaining 14 integer bits, then 16 zero
; fractional bits. No 64-bit temporary or general 64-bit divide is needed.
; If abs(b).lo==0, native DIV/MOD find the upper quotient word and only
; 16 one-word remainder steps remain. Remainders stay below abs(b)<=2^31;
; their left shifts therefore fit in the two-word representation.
FixedDiv:
    XOR T7, A1, A3
    SLT SC, A1, R0
    BEQ SC, R0, L_doom_div_a_magnitude
    SLTU T0, R0, A0
    SUB A0, R0, A0
    SUB A1, R0, A1
    SUB A1, A1, T0
L_doom_div_a_magnitude:
    SLT SC, A3, R0
    BEQ SC, R0, L_doom_div_b_magnitude
    SLTU T0, R0, A2
    SUB A2, R0, A2
    SUB A3, R0, A3
    SUB A3, A3, T0
L_doom_div_b_magnitude:
    SRLI T0, A0, 14
    SLLI T1, A1, 2
    OR T0, T0, T1
    SRLI T1, A1, 14
    SLTU SC, T1, A3
    BNE SC, R0, L_doom_div_calculate
    BNE T1, A3, L_doom_div_saturate
    SLTU SC, T0, A2
    BEQ SC, R0, L_doom_div_saturate
L_doom_div_calculate:
    BEQ A2, R0, L_doom_div_integer_denominator
    SLLI A0, A0, 2
    LI RV0, 0
    LI RV1, 0
    LI T6, 14
L_doom_div_integer_loop:
    SRLI T2, A0, 15
    SLLI A0, A0, 1
    SRLI SC, T0, 15
    SLLI T1, T1, 1
    OR T1, T1, SC
    SLLI T0, T0, 1
    OR T0, T0, T2
    SLLI RV0, RV0, 1
    SLTU SC, T1, A3
    BNE SC, R0, L_doom_div_integer_next
    BNE T1, A3, L_doom_div_integer_subtract
    SLTU SC, T0, A2
    BNE SC, R0, L_doom_div_integer_next
L_doom_div_integer_subtract:
    SLTU T2, T0, A2
    SUB T0, T0, A2
    SUB T1, T1, A3
    SUB T1, T1, T2
    ORI RV0, RV0, 1
L_doom_div_integer_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_div_integer_loop
    LI T6, 16
L_doom_div_fraction_loop:
    SRLI SC, T0, 15
    SLLI T1, T1, 1
    OR T1, T1, SC
    SLLI T0, T0, 1
    SRLI SC, RV0, 15
    SLLI RV1, RV1, 1
    OR RV1, RV1, SC
    SLLI RV0, RV0, 1
    SLTU SC, T1, A3
    BNE SC, R0, L_doom_div_fraction_next
    BNE T1, A3, L_doom_div_fraction_subtract
    SLTU SC, T0, A2
    BNE SC, R0, L_doom_div_fraction_next
L_doom_div_fraction_subtract:
    SLTU T2, T0, A2
    SUB T0, T0, A2
    SUB T1, T1, A3
    SUB T1, T1, T2
    ORI RV0, RV0, 1
L_doom_div_fraction_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_div_fraction_loop
L_doom_div_apply_sign:
    SLT SC, T7, R0
    BEQ SC, R0, L_doom_div_return
    SLTU SC, R0, RV0
    SUB RV0, R0, RV0
    SUB RV1, R0, RV1
    SUB RV1, RV1, SC
L_doom_div_return:
    JALR R0, RAB, RA

L_doom_div_integer_denominator:
    DIV RV1, A1, A3
    MOD T0, A1, A3
    ADD RV0, A0, R0
    LI T6, 16
L_doom_div_short_loop:
    SRLI SC, RV0, 15
    SLLI RV0, RV0, 1
    SLLI T0, T0, 1
    OR T0, T0, SC
    SLTU SC, T0, A3
    BNE SC, R0, L_doom_div_short_next
    SUB T0, T0, A3
    ORI RV0, RV0, 1
L_doom_div_short_next:
    ADDI T6, T6, -1
    BNE T6, R0, L_doom_div_short_loop
    BEQ R0, R0, L_doom_div_apply_sign

L_doom_div_saturate:
    SLT SC, T7, R0
    BNE SC, R0, L_doom_div_saturate_negative
    LI RV0, 65535
    LI RV1, 32767
    JALR R0, RAB, RA
L_doom_div_saturate_negative:
    LI RV0, 0
    LI RV1, 32768
    JALR R0, RAB, RA
