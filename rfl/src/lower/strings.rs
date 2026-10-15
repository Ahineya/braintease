//! Strings support for RFL lowering.

use super::*;

pub(super) const STR_DATA: i64 = 3;

pub(super) fn emit_str(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    bytes: &[u8],
) -> Result<(), CompileError> {
    let base = proc.slots[dest].offset;
    release_managed(builder, proc, frame, dest)?;
    if bytes.is_empty() {
        store_cell(builder, frame, base, Value::Constant(0))?;
        return store_cell(builder, frame, base + 1, Value::Constant(0));
    }
    let len = Value::Constant(bytes.len() as i64);
    let words = builder
        .build_binary(
            IrBinaryOp::Add,
            len.clone(),
            Value::Constant(STR_DATA),
            COUNT,
        )
        .ir()?;
    let size = as_i32(builder, Value::Temp(words))?;
    let raw = builder
        .build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty())
        .ir()?
        .unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_ALLOC)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    store_object_cell(builder, &obj, VEC_REFS, Value::Constant(1))?;
    store_count(builder, &obj, VEC_LEN, len)?;
    for (index, byte) in bytes.iter().enumerate() {
        store_object_cell(
            builder,
            &obj,
            STR_DATA + index as i64,
            Value::Constant(i64::from(*byte)),
        )?;
    }
    let header = gep(builder, frame, base)?;
    store_vec_ptr(builder, &header, &obj)
}

pub(super) fn emit_string_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    string: usize,
    index: usize,
) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, string)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bad = builder.new_label();
    let ok_ptr = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), bad, ok_ptr)
        .ir()?;
    builder.create_block(ok_ptr).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1)
        .ir()?;
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_BOUNDS)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let data = builder
        .build_binary(IrBinaryOp::Add, at, Value::Constant(STR_DATA), COUNT)
        .ir()?;
    let ptr = builder
        .build_pointer_offset(obj, Value::Temp(data), ptr_ty())
        .ir()?;
    let loaded = builder.build_load(ptr, IrType::I16).ir()?;
    let masked = builder
        .build_binary(
            IrBinaryOp::And,
            Value::Temp(loaded),
            Value::Constant(0xFF),
            IrType::I16,
        )
        .ir()?;
    let scalar = cast_value(builder, Value::Temp(masked), Ty::U16, Ty::I32)?;
    store_slot(builder, proc, frame, dest, scalar)
}

pub(super) fn emit_str_concat(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    lhs: usize,
    rhs: usize,
) -> Result<(), CompileError> {
    let a = load_fat(builder, frame, proc.slots[lhs].offset)?;
    let b = load_fat(builder, frame, proc.slots[rhs].offset)?;
    let obj = call_helper(builder, "rfl_string_concat", vec![a, b], ptr_ty())?;
    replace_string(builder, proc, frame, dest, obj)
}

pub(super) fn emit_str_slice(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    base: usize,
    start: usize,
    end: usize,
    inclusive: bool,
) -> Result<(), CompileError> {
    let source = load_fat(builder, frame, proc.slots[base].offset)?;
    let start = load_slot(builder, proc, frame, start)?;
    let end = load_slot(builder, proc, frame, end)?;
    let end = exclusive_end(builder, end, inclusive)?;
    let obj = call_helper(
        builder,
        "rfl_string_slice",
        vec![source, start, end],
        ptr_ty(),
    )?;
    replace_string(builder, proc, frame, dest, obj)
}

pub(super) fn emit_str_from_char(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
) -> Result<(), CompileError> {
    let ch = load_slot(builder, proc, frame, src)?;
    let obj = call_helper(builder, "rfl_string_char", vec![ch], ptr_ty())?;
    replace_string(builder, proc, frame, dest, obj)
}

fn replace_string(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    obj: Value,
) -> Result<(), CompileError> {
    // The new object is complete before releasing a possibly aliased input.
    release_managed(builder, proc, frame, dest)?;
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    let obj = match obj {
        Value::Temp(id) => fat_temp(id, BankTag::Mixed),
        other => other,
    };
    store_vec_ptr(builder, &header, &obj)
}

pub(super) fn emit_str_from_byte(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
) -> Result<(), CompileError> {
    let byte = load_slot(builder, proc, frame, src)?;
    let wide = cast_value(builder, byte, Ty::U8, Ty::U16)?;
    let obj = alloc_string(builder, Value::Constant(1))?;
    store_object_cell(builder, &obj, STR_DATA, wide)?;
    replace_string(builder, proc, frame, dest, obj)
}

pub(super) fn build_char_string(builder: &mut IrBuilder, ch: Value) -> Result<Value, CompileError> {
    let one = builder.new_label();
    let two = builder.new_label();
    let three = builder.new_label();
    let four = builder.new_label();
    let done = builder.new_label();
    let bytes = [
        builder.build_alloca(IrType::I16, None).ir()?,
        builder.build_alloca(IrType::I16, None).ir()?,
        builder.build_alloca(IrType::I16, None).ir()?,
        builder.build_alloca(IrType::I16, None).ir()?,
    ];
    let nslot = builder.build_alloca(IrType::I16, None).ir()?;
    let limit128 = i32_const(builder, 0x80)?;
    let below128 = builder
        .build_binary(IrBinaryOp::Ult, ch.clone(), limit128, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(below128), one, two)
        .ir()?;

    builder.create_block(one).ir()?;
    let low = builder
        .build_unary(IrUnaryOp::Trunc, ch.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Temp(low), bytes[0].clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Constant(1), nslot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(done).ir()?;

    builder.create_block(two).ir()?;
    let limit800 = i32_const(builder, 0x800)?;
    let below800 = builder
        .build_binary(IrBinaryOp::Ult, ch.clone(), limit800, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(below800), three, four)
        .ir()?;

    builder.create_block(three).ir()?;
    write_utf8(builder, &ch, 2, &bytes)?;
    builder
        .build_store(Value::Constant(2), nslot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(done).ir()?;

    builder.create_block(four).ir()?;
    let limit10000 = i32_const(builder, 0x10000)?;
    let below10000 = builder
        .build_binary(IrBinaryOp::Ult, ch.clone(), limit10000, IrType::I1)
        .ir()?;
    let wide = builder.new_label();
    let huge = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(below10000), wide, huge)
        .ir()?;
    builder.create_block(wide).ir()?;
    write_utf8(builder, &ch, 3, &bytes)?;
    builder
        .build_store(Value::Constant(3), nslot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(huge).ir()?;
    write_utf8(builder, &ch, 4, &bytes)?;
    builder
        .build_store(Value::Constant(4), nslot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(done).ir()?;

    builder.create_block(done).ir()?;
    let count = Value::Temp(builder.build_load(nslot, IrType::I16).ir()?);
    let obj = alloc_string(builder, count.clone())?;
    for (index, slot) in bytes.iter().enumerate() {
        let use_it = builder.new_label();
        let skip = builder.new_label();
        let needed = builder
            .build_binary(
                IrBinaryOp::Ugt,
                count.clone(),
                Value::Constant(index as i64),
                IrType::I1,
            )
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(needed), use_it, skip)
            .ir()?;
        builder.create_block(use_it).ir()?;
        let byte = Value::Temp(builder.build_load(slot.clone(), IrType::I16).ir()?);
        store_object_cell(builder, &obj, STR_DATA + index as i64, byte)?;
        builder.build_branch(skip).ir()?;
        builder.create_block(skip).ir()?;
    }
    Ok(obj)
}

/// UTF-8 lead and continuation bytes for a scalar that needs `width` bytes.
pub(super) fn write_utf8(
    builder: &mut IrBuilder,
    ch: &Value,
    width: i64,
    bytes: &[Value],
) -> Result<(), CompileError> {
    let marks: [i64; 4] = [0, 0xC0, 0xE0, 0xF0];
    let lead_mask: [i64; 4] = [0x7F, 0x1F, 0x0F, 0x07];
    let lead_shift = (width - 1) * 6;
    let lead = utf8_piece(
        builder,
        ch,
        lead_shift,
        lead_mask[(width - 1) as usize],
        marks[(width - 1) as usize],
    )?;
    builder
        .build_store(lead, bytes[0].clone(), IrType::I16)
        .ir()?;
    for index in 1..width {
        let shift = (width - 1 - index) * 6;
        let piece = utf8_piece(builder, ch, shift, 0x3F, 0x80)?;
        builder
            .build_store(piece, bytes[index as usize].clone(), IrType::I16)
            .ir()?;
    }
    Ok(())
}

pub(super) fn utf8_piece(
    builder: &mut IrBuilder,
    ch: &Value,
    shift: i64,
    mask: i64,
    mark: i64,
) -> Result<Value, CompileError> {
    let shifted = if shift == 0 {
        ch.clone()
    } else {
        let amount = i32_const(builder, shift)?;
        Value::Temp(
            builder
                .build_binary(IrBinaryOp::LShr, ch.clone(), amount, IrType::I32)
                .ir()?,
        )
    };
    let mask = i32_const(builder, mask)?;
    let masked = builder
        .build_binary(IrBinaryOp::And, shifted, mask, IrType::I32)
        .ir()?;
    let mark = i32_const(builder, mark)?;
    let marked = builder
        .build_binary(IrBinaryOp::Or, Value::Temp(masked), mark, IrType::I32)
        .ir()?;
    Ok(Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, Value::Temp(marked), IrType::I16)
            .ir()?,
    ))
}

pub(super) fn alloc_string(
    builder: &mut IrBuilder,
    byte_len: Value,
) -> Result<Value, CompileError> {
    let byte_len = as_i32(builder, byte_len)?;
    let words = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                byte_len.clone(),
                Value::Constant(STR_DATA),
                COUNT,
            )
            .ir()?,
    );
    let size = as_i32(builder, words)?;
    let raw = builder
        .build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty())
        .ir()?
        .unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_ALLOC)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    store_object_cell(builder, &obj, VEC_REFS, Value::Constant(1))?;
    store_count(builder, &obj, VEC_LEN, byte_len)?;
    Ok(obj)
}

pub(super) fn emit_string_cmp(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    op: BinOp,
    lhs: usize,
    rhs: usize,
) -> Result<(), CompileError> {
    let a = load_fat(builder, frame, proc.slots[lhs].offset)?;
    let b = load_fat(builder, frame, proc.slots[rhs].offset)?;
    let value = call_helper(builder, "rfl_string_equal", vec![a, b], IrType::I16)?;
    let value = builder
        .build_binary(
            if op == BinOp::Eq {
                IrBinaryOp::Ne
            } else {
                IrBinaryOp::Eq
            },
            value,
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    store_slot(builder, proc, frame, dest, Value::Temp(value))
}

pub(super) fn string_len(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<Value, CompileError> {
    let header = vec_header(builder, proc, frame, slot)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let none = builder.new_label();
    let some = builder.new_label();
    let done = builder.new_label();
    let out = builder.build_alloca(COUNT, None).ir()?;
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), none, some)
        .ir()?;
    builder.create_block(none).ir()?;
    builder
        .build_store(Value::Constant(0), out.clone(), COUNT)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(some).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    builder.build_store(len, out.clone(), COUNT).ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(Value::Temp(builder.build_load(out, COUNT).ir()?))
}

pub(super) fn string_byte(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    index: Value,
) -> Result<Value, CompileError> {
    let header = vec_header(builder, proc, frame, slot)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let at = builder
        .build_binary(IrBinaryOp::Add, index, Value::Constant(STR_DATA), COUNT)
        .ir()?;
    let ptr = builder
        .build_pointer_offset(obj, Value::Temp(at), ptr_ty())
        .ir()?;
    Ok(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?))
}

pub(super) fn emit_print_string(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    newline: bool,
) -> Result<(), CompileError> {
    let len = string_len(builder, proc, frame, slot)?;
    let index_slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), index_slot.clone(), COUNT)
        .ir()?;
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let index = builder.build_load(index_slot.clone(), COUNT).ir()?;
    let finished = builder
        .build_binary(IrBinaryOp::Eq, Value::Temp(index), len, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(finished), done, body)
        .ir()?;
    builder.create_block(body).ir()?;
    let byte = string_byte(builder, proc, frame, slot, Value::Temp(index))?;
    putchar(builder, byte)?;
    let stepped = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Temp(index),
            Value::Constant(1),
            COUNT,
        )
        .ir()?;
    builder
        .build_store(Value::Temp(stepped), index_slot, COUNT)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    if newline {
        putchar(builder, Value::Constant(10))?;
    }
    Ok(())
}
