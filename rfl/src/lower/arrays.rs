//! Arrays support for RFL lowering.

use super::*;

pub(super) fn exclusive_end(
    builder: &mut IrBuilder,
    end: Value,
    inclusive: bool,
) -> Result<Value, CompileError> {
    if !inclusive {
        return Ok(end);
    }
    let exclusive = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, end.clone(), Value::Constant(1), COUNT)
            .ir()?,
    );
    let wrapped = builder
        .build_binary(IrBinaryOp::Ule, exclusive.clone(), end, IrType::I1)
        .ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(wrapped), bad, ok)
        .ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_OVERFLOW)?;
    builder.create_block(ok).ir()?;
    Ok(exclusive)
}

pub(super) fn check_range(
    builder: &mut IrBuilder,
    start: Value,
    exclusive: Value,
    len: Value,
) -> Result<(), CompileError> {
    let bad = builder.new_label();
    let mid = builder.new_label();
    let ok = builder.new_label();
    let start_ok = builder
        .build_binary(IrBinaryOp::Ule, start, exclusive.clone(), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(start_ok), mid, bad)
        .ir()?;
    builder.create_block(mid).ir()?;
    let end_ok = builder
        .build_binary(IrBinaryOp::Ule, exclusive, len, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(end_ok), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_RANGE)?;
    builder.create_block(ok).ir()?;
    Ok(())
}

pub(super) fn store_empty_slice(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
) -> Result<(), CompileError> {
    for cell in 0..SLICE_CELLS {
        store_cell(builder, frame, offset + cell, Value::Constant(0))?;
    }
    Ok(())
}

pub(super) fn emit_array_lit(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    elems: &[usize],
    elem: Ty,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let mut at = proc.slots[dest].offset;
    for slot in elems {
        retain_managed(builder, proc, frame, *slot)?;
        for cell in 0..elem.cells() {
            let value = load_cell(builder, frame, proc.slots[*slot].offset + cell, IrType::I16)?;
            store_cell(builder, frame, at, value)?;
            at += 1;
        }
    }
    Ok(())
}

pub(super) fn copy_inline_elem(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    base: i64,
    index: Value,
    width: i64,
    dest: usize,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let scaled = scale_index(builder, index, width)?;
    let origin = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, Value::Constant(base), scaled, COUNT)
            .ir()?,
    );
    for cell in 0..width {
        let off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let ptr = builder
            .build_pointer_offset(frame.clone(), off, ptr_ty())
            .ir()?;
        let word = Value::Temp(builder.build_load(ptr, IrType::I16).ir()?);
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    retain_managed(builder, proc, frame, dest)
}

pub(super) fn store_inline_elem(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    value: usize,
    base: i64,
    index: Value,
    width: i64,
) -> Result<(), CompileError> {
    let scaled = scale_index(builder, index, width)?;
    let origin = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, Value::Constant(base), scaled, COUNT)
            .ir()?,
    );
    let place = builder
        .build_pointer_offset(frame.clone(), origin.clone(), ptr_ty())
        .ir()?;
    release_managed_through(builder, proc, &place, proc.slots[value].ty)?;
    retain_managed(builder, proc, frame, value)?;
    for cell in 0..width {
        let word = load_cell(builder, frame, proc.slots[value].offset + cell, IrType::I16)?;
        let off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let ptr = builder
            .build_pointer_offset(frame.clone(), off, ptr_ty())
            .ir()?;
        builder.build_store(word, ptr, IrType::I16).ir()?;
    }
    Ok(())
}

pub(super) fn emit_array_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    array: usize,
    index: usize,
) -> Result<(), CompileError> {
    let Ty::Array { len, width, .. } = proc.slots[array].ty else {
        return Ok(());
    };
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(
            IrBinaryOp::Ult,
            at.clone(),
            Value::Constant(len as i64),
            IrType::I1,
        )
        .ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_BOUNDS)?;
    builder.create_block(ok).ir()?;
    copy_inline_elem(
        builder,
        proc,
        frame,
        proc.slots[array].offset,
        at,
        width as i64,
        dest,
    )
}

pub(super) fn emit_array_store_at(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    base: i64,
    len: u16,
    width: u16,
    index: usize,
    value: usize,
) -> Result<(), CompileError> {
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(
            IrBinaryOp::Ult,
            at.clone(),
            Value::Constant(len as i64),
            IrType::I1,
        )
        .ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_BOUNDS)?;
    builder.create_block(ok).ir()?;
    store_inline_elem(builder, proc, frame, value, base, at, width as i64)
}

pub(super) fn emit_array_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    array: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let Ty::Array { len, width, .. } = proc.slots[array].ty else {
        return Ok(());
    };
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(
            IrBinaryOp::Ult,
            at.clone(),
            Value::Constant(len as i64),
            IrType::I1,
        )
        .ir()?;
    let none = builder.new_label();
    let ok = builder.new_label();
    let done = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, none)
        .ir()?;
    builder.create_block(ok).ir()?;
    copy_inline_elem(
        builder,
        proc,
        frame,
        proc.slots[array].offset,
        at,
        width as i64,
        value,
    )?;
    emit_make(
        builder,
        proc,
        frame,
        dest,
        some_tag,
        &[(value, elem.cells())],
    )?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn emit_slice_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    slice: usize,
    index: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    let base = proc.slots[slice].offset;
    let len = load_count_at(builder, frame, base + SLICE_LEN)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1)
        .ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_BOUNDS)?;
    builder.create_block(ok).ir()?;
    let start = load_count_at(builder, frame, base + SLICE_START)?;
    let elem_at = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, start, at, COUNT)
            .ir()?,
    );
    let addr = load_cell(builder, frame, base, IrType::I16)?;
    let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let scaled = scale_index(builder, elem_at, elem.cells())?;
    let data = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
            .ir()?,
    );
    copy_object_cells(builder, proc, frame, &obj, data, dest, elem.cells())
}

pub(super) fn emit_slice_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slice: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let base = proc.slots[slice].offset;
    let len = load_count_at(builder, frame, base + SLICE_LEN)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1)
        .ir()?;
    let none = builder.new_label();
    let ok = builder.new_label();
    let done = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, none)
        .ir()?;
    builder.create_block(ok).ir()?;
    let start = load_count_at(builder, frame, base + SLICE_START)?;
    let elem_at = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, start, at, COUNT)
            .ir()?,
    );
    let addr = load_cell(builder, frame, base, IrType::I16)?;
    let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let scaled = scale_index(builder, elem_at, elem.cells())?;
    let data = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
            .ir()?,
    );
    copy_object_cells(builder, proc, frame, &obj, data, value, elem.cells())?;
    emit_make(
        builder,
        proc,
        frame,
        dest,
        some_tag,
        &[(value, elem.cells())],
    )?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn emit_slice_new(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    base: usize,
    start: usize,
    end: usize,
    inclusive: bool,
    elem: Ty,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let start = load_slot(builder, proc, frame, start)?;
    let end = load_slot(builder, proc, frame, end)?;
    let exclusive = exclusive_end(builder, end, inclusive)?;
    match proc.slots[base].ty {
        Ty::Array { len, width, .. } => {
            check_range(
                builder,
                start.clone(),
                exclusive.clone(),
                Value::Constant(len as i64),
            )?;
            let count = Value::Temp(
                builder
                    .build_binary(IrBinaryOp::Sub, exclusive, start.clone(), COUNT)
                    .ir()?,
            );
            slice_from_array(builder, proc, frame, dest, base, start, count, width as i64)
        }
        Ty::Slice(_) => slice_from_slice(builder, proc, frame, dest, base, start, exclusive),
        _ => slice_from_vec(builder, proc, frame, dest, base, start, exclusive, elem),
    }
}

pub(super) fn slice_from_array(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    array: usize,
    start: Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let empty = builder.new_label();
    let copy = builder.new_label();
    let is_empty = builder
        .build_binary(
            IrBinaryOp::Eq,
            count.clone(),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_empty), empty, copy)
        .ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    let done = builder.new_label();
    builder.build_branch(done).ir()?;
    builder.create_block(copy).ir()?;
    let obj = alloc_vec(builder, count.clone(), count.clone(), width)?;
    copy_array_span(
        builder,
        frame,
        proc.slots[array].offset,
        start,
        &obj,
        count.clone(),
        width,
    )?;
    retain_buffer_elements(
        builder,
        proc,
        &obj,
        count.clone(),
        proc.collections.element(proc.slots[array].ty),
    )?;
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    store_vec_ptr(builder, &header, &obj)?;
    store_count_at(
        builder,
        frame,
        proc.slots[dest].offset + SLICE_START,
        Value::Constant(0),
    )?;
    store_count_at(builder, frame, proc.slots[dest].offset + SLICE_LEN, count)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn slice_from_slice(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    start: Value,
    exclusive: Value,
) -> Result<(), CompileError> {
    let base = proc.slots[src].offset;
    let old_len = load_count_at(builder, frame, base + SLICE_LEN)?;
    check_range(builder, start.clone(), exclusive.clone(), old_len)?;
    let count = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, exclusive, start.clone(), COUNT)
            .ir()?,
    );
    let old_start = load_count_at(builder, frame, base + SLICE_START)?;
    let new_start = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, old_start, start, COUNT)
            .ir()?,
    );
    let empty = builder.new_label();
    let keep = builder.new_label();
    let done = builder.new_label();
    let is_empty = builder
        .build_binary(
            IrBinaryOp::Eq,
            count.clone(),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_empty), empty, keep)
        .ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(keep).ir()?;
    for cell in 0..2 {
        let word = load_cell(builder, frame, base + cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    store_count_at(
        builder,
        frame,
        proc.slots[dest].offset + SLICE_START,
        new_start,
    )?;
    store_count_at(builder, frame, proc.slots[dest].offset + SLICE_LEN, count)?;
    retain_vec(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn slice_from_vec(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
    start: Value,
    exclusive: Value,
    _elem: Ty,
) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let len_slot = builder.build_alloca(COUNT, None).ir()?;
    let missing = builder.new_label();
    let present = builder.new_label();
    let join = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), missing, present)
        .ir()?;
    builder.create_block(missing).ir()?;
    builder
        .build_store(Value::Constant(0), len_slot.clone(), COUNT)
        .ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(present).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    builder.build_store(len, len_slot.clone(), COUNT).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(join).ir()?;
    let len = Value::Temp(builder.build_load(len_slot, COUNT).ir()?);
    check_range(builder, start.clone(), exclusive.clone(), len)?;
    let count = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, exclusive, start.clone(), COUNT)
            .ir()?,
    );
    let empty = builder.new_label();
    let keep = builder.new_label();
    let done = builder.new_label();
    let is_empty = builder
        .build_binary(
            IrBinaryOp::Eq,
            count.clone(),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_empty), empty, keep)
        .ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(keep).ir()?;
    let header = vec_header(builder, proc, frame, vec)?;
    for cell in 0..2 {
        let word = load_cell(builder, &header, cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    store_count_at(builder, frame, proc.slots[dest].offset + SLICE_START, start)?;
    store_count_at(builder, frame, proc.slots[dest].offset + SLICE_LEN, count)?;
    retain_vec(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn copy_array_span(
    builder: &mut IrBuilder,
    frame: &Value,
    array_offset: i64,
    start: Value,
    obj: &Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    let i_slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), i_slot.clone(), COUNT)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(i_slot.clone(), COUNT).ir()?);
    let more = builder
        .build_binary(IrBinaryOp::Ult, i.clone(), count.clone(), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let from = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, start.clone(), i.clone(), COUNT)
            .ir()?,
    );
    let src_scaled = scale_index(builder, from, width)?;
    let src_origin = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                Value::Constant(array_offset),
                src_scaled,
                COUNT,
            )
            .ir()?,
    );
    let dst_scaled = scale_index(builder, i.clone(), width)?;
    let dst_origin = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                dst_scaled,
                Value::Constant(VEC_DATA),
                COUNT,
            )
            .ir()?,
    );
    for cell in 0..width {
        let src_off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    src_origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let src_ptr = builder
            .build_pointer_offset(frame.clone(), src_off, ptr_ty())
            .ir()?;
        let word = Value::Temp(builder.build_load(src_ptr, IrType::I16).ir()?);
        let dst_off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    dst_origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let dst_ptr = builder
            .build_pointer_offset(obj.clone(), dst_off, ptr_ty())
            .ir()?;
        builder.build_store(word, dst_ptr, IrType::I16).ir()?;
    }
    let next = builder
        .build_binary(IrBinaryOp::Add, i, Value::Constant(1), COUNT)
        .ir()?;
    builder.build_store(Value::Temp(next), i_slot, COUNT).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn emit_to_vec(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    match proc.slots[src].ty {
        Ty::Array { len, width, .. } => {
            if len == 0 {
                store_cell(builder, frame, proc.slots[dest].offset, Value::Constant(0))?;
                return store_cell(
                    builder,
                    frame,
                    proc.slots[dest].offset + 1,
                    Value::Constant(0),
                );
            }
            let count = Value::Constant(len as i64);
            let obj = alloc_vec(builder, count.clone(), count.clone(), width as i64)?;
            copy_array_span(
                builder,
                frame,
                proc.slots[src].offset,
                Value::Constant(0),
                &obj,
                count.clone(),
                width as i64,
            )?;
            retain_buffer_elements(builder, proc, &obj, count, elem)?;
            store_vec_ptr(builder, &header, &obj)
        }
        Ty::Slice(_) => {
            let base = proc.slots[src].offset;
            let len = load_count_at(builder, frame, base + SLICE_LEN)?;
            let empty = builder.new_label();
            let copy = builder.new_label();
            let done = builder.new_label();
            let is_empty = builder
                .build_binary(IrBinaryOp::Eq, len.clone(), Value::Constant(0), IrType::I1)
                .ir()?;
            builder
                .build_branch_cond(Value::Temp(is_empty), empty, copy)
                .ir()?;
            builder.create_block(empty).ir()?;
            store_cell(builder, frame, proc.slots[dest].offset, Value::Constant(0))?;
            store_cell(
                builder,
                frame,
                proc.slots[dest].offset + 1,
                Value::Constant(0),
            )?;
            builder.build_branch(done).ir()?;
            builder.create_block(copy).ir()?;
            let obj = alloc_vec(builder, len.clone(), len.clone(), elem.cells())?;
            let start = load_count_at(builder, frame, base + SLICE_START)?;
            let addr = load_cell(builder, frame, base, IrType::I16)?;
            let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
            let src_obj = fat_from_parts(builder, addr, bank)?;
            copy_object_span(builder, &src_obj, start, &obj, len.clone(), elem.cells())?;
            retain_buffer_elements(builder, proc, &obj, len, elem)?;
            store_vec_ptr(builder, &header, &obj)?;
            builder.build_branch(done).ir()?;
            builder.create_block(done).ir()?;
            Ok(())
        }
        _ => Ok(()),
    }
}

pub(super) fn copy_object_span(
    builder: &mut IrBuilder,
    src: &Value,
    start: Value,
    dest: &Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    let i_slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), i_slot.clone(), COUNT)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(i_slot.clone(), COUNT).ir()?);
    let more = builder
        .build_binary(IrBinaryOp::Ult, i.clone(), count.clone(), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let from = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, start.clone(), i.clone(), COUNT)
            .ir()?,
    );
    let src_scaled = scale_index(builder, from, width)?;
    let src_origin = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                src_scaled,
                Value::Constant(VEC_DATA),
                COUNT,
            )
            .ir()?,
    );
    let dst_scaled = scale_index(builder, i.clone(), width)?;
    let dst_origin = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                dst_scaled,
                Value::Constant(VEC_DATA),
                COUNT,
            )
            .ir()?,
    );
    for cell in 0..width {
        let src_off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    src_origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let src_ptr = builder
            .build_pointer_offset(src.clone(), src_off, ptr_ty())
            .ir()?;
        let word = Value::Temp(builder.build_load(src_ptr, IrType::I16).ir()?);
        let dst_off = Value::Temp(
            builder
                .build_binary(
                    IrBinaryOp::Add,
                    dst_origin.clone(),
                    Value::Constant(cell),
                    COUNT,
                )
                .ir()?,
        );
        let dst_ptr = builder
            .build_pointer_offset(dest.clone(), dst_off, ptr_ty())
            .ir()?;
        builder.build_store(word, dst_ptr, IrType::I16).ir()?;
    }
    let next = builder
        .build_binary(IrBinaryOp::Add, i, Value::Constant(1), COUNT)
        .ir()?;
    builder.build_store(Value::Temp(next), i_slot, COUNT).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}
