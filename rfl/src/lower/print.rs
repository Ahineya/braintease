//! Print support for RFL lowering.

use super::*;

pub(super) fn emit_print(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    ty: Ty,
    newline: bool,
) -> Result<(), CompileError> {
    if ty.is_float() {
        let value = load_slot(builder, proc, frame, slot)?;
        let bits = if ty == Ty::F32 {
            call_helper(builder, "__rcc_f64_from_f32", vec![value], IrType::I64)?
        } else {
            value
        };
        builder
            .build_call(
                Value::Global("rfl_print_f64".to_string()),
                vec![bits],
                IrType::Void,
            )
            .ir()?;
        if newline {
            putchar(builder, Value::Constant(10))?;
        }
        return Ok(());
    }
    if ty == Ty::Char {
        let value = load_slot(builder, proc, frame, slot)?;
        let cell = builder
            .build_unary(IrUnaryOp::Trunc, value, IrType::I16)
            .ir()?;
        putchar(builder, Value::Temp(cell))?;
        if newline {
            putchar(builder, Value::Constant(10))?;
        }
        return Ok(());
    }
    if ty == Ty::String {
        return emit_print_string(builder, proc, frame, slot, newline);
    }
    if ty == Ty::Bool {
        let value = load_slot(builder, proc, frame, slot)?;
        let yes = builder.new_label();
        let no = builder.new_label();
        let after = builder.new_label();
        let is_false = builder
            .build_binary(IrBinaryOp::Eq, value, Value::Constant(0), IrType::I1)
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(is_false), no, yes)
            .ir()?;
        builder.create_block(yes).ir()?;
        emit_chars(builder, &['t' as u16, 'r' as u16, 'u' as u16, 'e' as u16])?;
        builder.build_branch(after).ir()?;
        builder.create_block(no).ir()?;
        emit_chars(
            builder,
            &['f' as u16, 'a' as u16, 'l' as u16, 's' as u16, 'e' as u16],
        )?;
        builder.build_branch(after).ir()?;
        builder.create_block(after).ir()?;
        if newline {
            putchar(builder, Value::Constant(10))?;
        }
        return Ok(());
    }
    let value = load_slot(builder, proc, frame, slot)?;
    let signed = ty.is_signed();
    let bits = cast_value(builder, value, ty, if signed { Ty::I64 } else { Ty::U64 })?;
    if signed {
        let zero = wide_zero(builder, IrType::I64)?;
        let neg = builder
            .build_binary(IrBinaryOp::Slt, bits.clone(), zero.clone(), IrType::I1)
            .ir()?;
        let minus = builder.new_label();
        let plain = builder.new_label();
        let after = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(neg), minus, plain)
            .ir()?;
        builder.create_block(minus).ir()?;
        putchar(builder, Value::Constant('-' as i64))?;
        let flipped = builder
            .build_binary(IrBinaryOp::Sub, zero, bits.clone(), IrType::I64)
            .ir()?;
        builder
            .build_call(
                Value::Global("rfl_print_u64".to_string()),
                vec![Value::Temp(flipped)],
                IrType::Void,
            )
            .ir()?;
        builder.build_branch(after).ir()?;
        builder.create_block(plain).ir()?;
        builder
            .build_call(
                Value::Global("rfl_print_u64".to_string()),
                vec![bits],
                IrType::Void,
            )
            .ir()?;
        builder.build_branch(after).ir()?;
        builder.create_block(after).ir()?;
    } else {
        builder
            .build_call(
                Value::Global("rfl_print_u64".to_string()),
                vec![bits],
                IrType::Void,
            )
            .ir()?;
    }
    if newline {
        putchar(builder, Value::Constant(10))?;
    }
    Ok(())
}

pub(super) fn emit_chars(builder: &mut IrBuilder, chars: &[u16]) -> Result<(), CompileError> {
    for ch in chars {
        putchar(builder, Value::Constant(*ch as i64))?;
    }
    Ok(())
}

pub(super) fn putchar(builder: &mut IrBuilder, value: Value) -> Result<(), CompileError> {
    builder
        .build_call(
            Value::Global("putchar".to_string()),
            vec![value],
            IrType::Void,
        )
        .ir()?;
    Ok(())
}

/// Unsigned decimal. The caller prints a minus and passes the magnitude.
pub(super) fn emit_print_u64(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_print_u64".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I64);
    let entry = builder.new_label();
    let step = builder.new_label();
    let dump = builder.new_label();
    let emit_digit = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let buf = builder
        .build_alloca(IrType::I16, Some(Value::Constant(24)))
        .ir()?;
    let n_slot = builder.build_alloca(IrType::I64, None).ir()?;
    let i_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder
        .build_store(Value::Temp(0), n_slot.clone(), IrType::I64)
        .ir()?;
    builder
        .build_store(Value::Constant(0), i_slot.clone(), IrType::I16)
        .ir()?;
    let small = builder.new_label();
    let ten = i64_const(builder, 10)?;
    let is_small = builder
        .build_binary(IrBinaryOp::Ult, Value::Temp(0), ten, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_small), small, step)
        .ir()?;
    builder.create_block(small).ir()?;
    let low = builder
        .build_unary(IrUnaryOp::Trunc, Value::Temp(0), IrType::I16)
        .ir()?;
    let digit = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Temp(low),
            Value::Constant(48),
            IrType::I16,
        )
        .ir()?;
    putchar(builder, Value::Temp(digit))?;
    builder.build_branch(done).ir()?;

    builder.create_block(step).ir()?;
    let n = Value::Temp(builder.build_load(n_slot.clone(), IrType::I64).ir()?);
    let ten = i64_const(builder, 10)?;
    let next = Value::Temp(
        builder
            .build_binary(IrBinaryOp::UDiv, n.clone(), ten.clone(), IrType::I64)
            .ir()?,
    );
    let consumed = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Mul, next.clone(), ten, IrType::I64)
            .ir()?,
    );
    let digit = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, n, consumed, IrType::I64)
            .ir()?,
    );
    builder
        .build_store(next.clone(), n_slot.clone(), IrType::I64)
        .ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let cell = builder
        .build_pointer_offset(buf.clone(), index.clone(), ptr_ty())
        .ir()?;
    let low = Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, digit, IrType::I16)
            .ir()?,
    );
    builder.build_store(low, cell, IrType::I16).ir()?;
    let bumped = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, index, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(bumped, i_slot.clone(), IrType::I16)
        .ir()?;
    let zero = wide_zero(builder, IrType::I64)?;
    let finished = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, next, zero, IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(finished, dump, step).ir()?;

    builder.create_block(dump).ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let empty = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, index, Value::Constant(0), IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(empty, done, emit_digit).ir()?;

    builder.create_block(emit_digit).ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let prev = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, index, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(prev.clone(), i_slot.clone(), IrType::I16)
        .ir()?;
    let cell = builder.build_pointer_offset(buf, prev, ptr_ty()).ir()?;
    let digit = Value::Temp(builder.build_load(cell, IrType::I16).ir()?);
    let ch = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, digit, Value::Constant(48), IrType::I16)
            .ir()?,
    );
    putchar(builder, ch)?;
    builder.build_branch(dump).ir()?;

    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

/// Decimal for `f32` and `f64`. Six fractional digits, trailing zeros removed.
/// Values outside `1e-4 .. 1e7` use scientific form. `nan` and `inf` are named.
pub(super) fn emit_print_f64(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_print_f64".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I64);
    let entry = builder.new_label();
    let special = builder.new_label();
    let inf = builder.new_label();
    let nan = builder.new_label();
    let minus_inf = builder.new_label();
    let plus_inf = builder.new_label();
    let finite = builder.new_label();
    let zero = builder.new_label();
    let nonzero = builder.new_label();
    let wrote_sign = builder.new_label();
    let sci = builder.new_label();
    let maybe_tiny = builder.new_label();
    let fixed = builder.new_label();
    let norm_hi = builder.new_label();
    let div10 = builder.new_label();
    let norm_lo = builder.new_label();
    let mul10 = builder.new_label();
    let round = builder.new_label();
    let carry = builder.new_label();
    let whole = builder.new_label();
    let frac = builder.new_label();
    let scan = builder.new_label();
    let emit_digit = builder.new_label();
    let after_frac = builder.new_label();
    let exponent = builder.new_label();
    let exp_neg = builder.new_label();
    let exp_pos = builder.new_label();
    let exp_num = builder.new_label();
    let done = builder.new_label();

    builder.create_block(entry).ir()?;
    let bits = Value::Temp(0);
    let exp_mask = i64_const(builder, 0x7ff0_0000_0000_0000)?;
    let frac_mask = i64_const(builder, 0x000f_ffff_ffff_ffff)?;
    let abs_mask = i64_const(builder, 0x7fff_ffff_ffff_ffff)?;
    let zero64 = wide_zero(builder, IrType::I64)?;
    let one64 = i64_const(builder, 1)?;
    let ten64 = i64_const(builder, 10)?;
    let million = i64_const(builder, 1_000_000)?;
    let f_zero = f64_bits(builder, 0.0)?;
    let f_10 = f64_bits(builder, 10.0)?;
    let f_1 = f64_bits(builder, 1.0)?;
    let f_huge = f64_bits(builder, 1.0e7)?;
    let f_tiny = f64_bits(builder, 1.0e-4)?;
    let f_scale = f64_bits(builder, 1_000_000.0)?;
    let f_half = f64_bits(builder, 0.5)?;
    let biased = Value::Temp(
        builder
            .build_binary(IrBinaryOp::And, bits.clone(), exp_mask.clone(), IrType::I64)
            .ir()?,
    );
    let payload = Value::Temp(
        builder
            .build_binary(IrBinaryOp::And, bits.clone(), frac_mask, IrType::I64)
            .ir()?,
    );
    let is_special = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, biased, exp_mask, IrType::I1)
            .ir()?,
    );
    builder
        .build_branch_cond(is_special, special, finite)
        .ir()?;

    builder.create_block(special).ir()?;
    let frac_zero = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, payload, zero64.clone(), IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(frac_zero, inf, nan).ir()?;

    builder.create_block(nan).ir()?;
    emit_chars(builder, &['n' as u16, 'a' as u16, 'n' as u16])?;
    builder.build_branch(done).ir()?;

    builder.create_block(inf).ir()?;
    let negative = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Slt, bits.clone(), zero64.clone(), IrType::I1)
            .ir()?,
    );
    builder
        .build_branch_cond(negative, minus_inf, plus_inf)
        .ir()?;
    builder.create_block(minus_inf).ir()?;
    putchar(builder, Value::Constant('-' as i64))?;
    builder.build_branch(plus_inf).ir()?;
    builder.create_block(plus_inf).ir()?;
    emit_chars(builder, &['i' as u16, 'n' as u16, 'f' as u16])?;
    builder.build_branch(done).ir()?;

    builder.create_block(finite).ir()?;
    let mag_slot = builder.build_alloca(IrType::I64, None).ir()?;
    let exp_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let show_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let whole_slot = builder.build_alloca(IrType::I64, None).ir()?;
    let frac_slot = builder.build_alloca(IrType::I64, None).ir()?;
    let buf = builder
        .build_alloca(IrType::I16, Some(Value::Constant(6)))
        .ir()?;
    let index_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let count_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let magnitude = Value::Temp(
        builder
            .build_binary(IrBinaryOp::And, bits.clone(), abs_mask, IrType::I64)
            .ir()?,
    );
    builder
        .build_store(magnitude.clone(), mag_slot.clone(), IrType::I64)
        .ir()?;
    let is_zero = f64_flag(builder, "eq", magnitude, f_zero)?;
    builder.build_branch_cond(is_zero, zero, nonzero).ir()?;

    builder.create_block(zero).ir()?;
    putchar(builder, Value::Constant('0' as i64))?;
    builder.build_branch(done).ir()?;

    builder.create_block(nonzero).ir()?;
    let negative = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Slt, bits, zero64.clone(), IrType::I1)
            .ir()?,
    );
    builder
        .build_branch_cond(negative, wrote_sign, maybe_tiny)
        .ir()?;
    builder.create_block(wrote_sign).ir()?;
    putchar(builder, Value::Constant('-' as i64))?;
    builder.build_branch(maybe_tiny).ir()?;

    builder.create_block(maybe_tiny).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let huge = f64_flag(builder, "ge", mag.clone(), f_huge)?;
    let tiny_label = builder.new_label();
    builder.build_branch_cond(huge, sci, tiny_label).ir()?;
    builder.create_block(tiny_label).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let tiny = f64_flag(builder, "lt", mag, f_tiny)?;
    builder.build_branch_cond(tiny, sci, fixed).ir()?;

    builder.create_block(fixed).ir()?;
    builder
        .build_store(Value::Constant(0), exp_slot.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Constant(0), show_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(round).ir()?;

    builder.create_block(sci).ir()?;
    builder
        .build_store(Value::Constant(0), exp_slot.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Constant(1), show_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(norm_hi).ir()?;

    builder.create_block(norm_hi).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let over = f64_flag(builder, "ge", mag, f_10.clone())?;
    builder.build_branch_cond(over, div10, norm_lo).ir()?;
    builder.create_block(div10).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let next = f64_bin(builder, "div", mag, f_10.clone())?;
    builder
        .build_store(next, mag_slot.clone(), IrType::I64)
        .ir()?;
    let exp = load_i16(builder, &exp_slot)?;
    let bumped = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, exp, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(bumped, exp_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(norm_hi).ir()?;

    builder.create_block(norm_lo).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let under = f64_flag(builder, "lt", mag, f_1.clone())?;
    builder.build_branch_cond(under, mul10, round).ir()?;
    builder.create_block(mul10).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let next = f64_bin(builder, "mul", mag, f_10)?;
    builder
        .build_store(next, mag_slot.clone(), IrType::I64)
        .ir()?;
    let exp = load_i16(builder, &exp_slot)?;
    let bumped = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, exp, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(bumped, exp_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(norm_lo).ir()?;

    builder.create_block(round).ir()?;
    let mag = load_i64(builder, &mag_slot)?;
    let scaled = f64_bin(builder, "mul", mag, f_scale)?;
    let rounded = f64_bin(builder, "add", scaled, f_half)?;
    let as_int = call_helper(builder, "__rcc_f64_to_i64", vec![rounded], IrType::I64)?;
    let ip = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::UDiv,
                as_int.clone(),
                million.clone(),
                IrType::I64,
            )
            .ir()?,
    );
    let consumed = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Mul, ip.clone(), million, IrType::I64)
            .ir()?,
    );
    let rem = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, as_int, consumed, IrType::I64)
            .ir()?,
    );
    builder
        .build_store(ip, whole_slot.clone(), IrType::I64)
        .ir()?;
    builder
        .build_store(rem, frac_slot.clone(), IrType::I64)
        .ir()?;
    let shown = load_i16(builder, &show_slot)?;
    let showing = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ne, shown, Value::Constant(0), IrType::I1)
            .ir()?,
    );
    let ip = load_i64(builder, &whole_slot)?;
    let overflowed = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Uge, ip, ten64, IrType::I1)
            .ir()?,
    );
    let do_carry = Value::Temp(
        builder
            .build_binary(IrBinaryOp::And, showing, overflowed, IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(do_carry, carry, whole).ir()?;

    builder.create_block(carry).ir()?;
    builder
        .build_store(one64, whole_slot.clone(), IrType::I64)
        .ir()?;
    builder
        .build_store(zero64.clone(), frac_slot.clone(), IrType::I64)
        .ir()?;
    let exp = load_i16(builder, &exp_slot)?;
    let bumped = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, exp, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(bumped, exp_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(whole).ir()?;

    builder.create_block(whole).ir()?;
    let ip = load_i64(builder, &whole_slot)?;
    builder
        .build_call(
            Value::Global("rfl_print_u64".to_string()),
            vec![ip],
            IrType::Void,
        )
        .ir()?;
    let rem = load_i64(builder, &frac_slot)?;
    let no_frac = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, rem.clone(), zero64, IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(no_frac, after_frac, frac).ir()?;

    builder.create_block(frac).ir()?;
    putchar(builder, Value::Constant('.' as i64))?;
    let mut rest = Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, rem, IrType::I32)
            .ir()?,
    );
    for (index, power) in [100_000_i64, 10_000, 1_000, 100, 10, 1]
        .into_iter()
        .enumerate()
    {
        let scale = i32_const(builder, power)?;
        let digit = Value::Temp(
            builder
                .build_binary(IrBinaryOp::UDiv, rest.clone(), scale.clone(), IrType::I32)
                .ir()?,
        );
        let consumed = Value::Temp(
            builder
                .build_binary(IrBinaryOp::Mul, digit.clone(), scale, IrType::I32)
                .ir()?,
        );
        rest = Value::Temp(
            builder
                .build_binary(IrBinaryOp::Sub, rest, consumed, IrType::I32)
                .ir()?,
        );
        let narrow = Value::Temp(
            builder
                .build_unary(IrUnaryOp::Trunc, digit, IrType::I16)
                .ir()?,
        );
        let cell = builder
            .build_pointer_offset(buf.clone(), Value::Constant(index as i64), ptr_ty())
            .ir()?;
        builder.build_store(narrow, cell, IrType::I16).ir()?;
    }
    builder
        .build_store(Value::Constant(6), index_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(scan).ir()?;

    builder.create_block(scan).ir()?;
    let index = load_i16(builder, &index_slot)?;
    let exhausted = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Eq,
                index.clone(),
                Value::Constant(0),
                IrType::I1,
            )
            .ir()?,
    );
    let look = builder.new_label();
    builder
        .build_branch_cond(exhausted, after_frac, look)
        .ir()?;
    builder.create_block(look).ir()?;
    let index = load_i16(builder, &index_slot)?;
    let prev = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, index, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(prev.clone(), index_slot.clone(), IrType::I16)
        .ir()?;
    let cell = builder
        .build_pointer_offset(buf.clone(), prev.clone(), ptr_ty())
        .ir()?;
    let digit = Value::Temp(builder.build_load(cell, IrType::I16).ir()?);
    let nonzero = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ne, digit, Value::Constant(0), IrType::I1)
            .ir()?,
    );
    let keep = builder.new_label();
    builder.build_branch_cond(nonzero, keep, scan).ir()?;
    builder.create_block(keep).ir()?;
    let index = load_i16(builder, &index_slot)?;
    let count = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, index, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(count, count_slot.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Constant(0), index_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(emit_digit).ir()?;

    builder.create_block(emit_digit).ir()?;
    let index = load_i16(builder, &index_slot)?;
    let count = load_i16(builder, &count_slot)?;
    let finished = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, index.clone(), count, IrType::I1)
            .ir()?,
    );
    let step = builder.new_label();
    builder.build_branch_cond(finished, after_frac, step).ir()?;
    builder.create_block(step).ir()?;
    let index = load_i16(builder, &index_slot)?;
    let cell = builder
        .build_pointer_offset(buf.clone(), index.clone(), ptr_ty())
        .ir()?;
    let digit = Value::Temp(builder.build_load(cell, IrType::I16).ir()?);
    let ch = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, digit, Value::Constant(48), IrType::I16)
            .ir()?,
    );
    putchar(builder, ch)?;
    let next = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, index, Value::Constant(1), IrType::I16)
            .ir()?,
    );
    builder
        .build_store(next, index_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(emit_digit).ir()?;

    builder.create_block(after_frac).ir()?;
    let shown = load_i16(builder, &show_slot)?;
    let showing = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ne, shown, Value::Constant(0), IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(showing, exponent, done).ir()?;

    builder.create_block(exponent).ir()?;
    putchar(builder, Value::Constant('e' as i64))?;
    let exp = load_i16(builder, &exp_slot)?;
    let negative = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Slt, exp, Value::Constant(0), IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(negative, exp_neg, exp_pos).ir()?;
    builder.create_block(exp_neg).ir()?;
    putchar(builder, Value::Constant('-' as i64))?;
    let exp = load_i16(builder, &exp_slot)?;
    let flipped = Value::Temp(builder.build_unary(IrUnaryOp::Neg, exp, IrType::I16).ir()?);
    builder
        .build_store(flipped, exp_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(exp_num).ir()?;
    builder.create_block(exp_pos).ir()?;
    putchar(builder, Value::Constant('+' as i64))?;
    builder.build_branch(exp_num).ir()?;
    builder.create_block(exp_num).ir()?;
    let exp = load_i16(builder, &exp_slot)?;
    let wide = Value::Temp(
        builder
            .build_unary(IrUnaryOp::SExt, exp, IrType::I64)
            .ir()?,
    );
    builder
        .build_call(
            Value::Global("rfl_print_u64".to_string()),
            vec![wide],
            IrType::Void,
        )
        .ir()?;
    builder.build_branch(done).ir()?;

    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

pub(super) fn f64_bits(builder: &mut IrBuilder, value: f64) -> Result<Value, CompileError> {
    i64_const(builder, value.to_bits() as i64)
}

pub(super) fn f64_flag(
    builder: &mut IrBuilder,
    op: &str,
    lhs: Value,
    rhs: Value,
) -> Result<Value, CompileError> {
    let raw = call_helper(
        builder,
        &format!("__rcc_f64_{op}"),
        vec![lhs, rhs],
        IrType::I16,
    )?;
    let flag = builder
        .build_binary(IrBinaryOp::Ne, raw, Value::Constant(0), IrType::I1)
        .ir()?;
    Ok(Value::Temp(flag))
}

pub(super) fn f64_bin(
    builder: &mut IrBuilder,
    op: &str,
    lhs: Value,
    rhs: Value,
) -> Result<Value, CompileError> {
    call_helper(
        builder,
        &format!("__rcc_f64_{op}"),
        vec![lhs, rhs],
        IrType::I64,
    )
}

pub(super) fn load_i64(builder: &mut IrBuilder, slot: &Value) -> Result<Value, CompileError> {
    Ok(Value::Temp(
        builder.build_load(slot.clone(), IrType::I64).ir()?,
    ))
}

pub(super) fn load_i16(builder: &mut IrBuilder, slot: &Value) -> Result<Value, CompileError> {
    Ok(Value::Temp(
        builder.build_load(slot.clone(), IrType::I16).ir()?,
    ))
}
