//! Vectors support for RFL lowering.

use super::*;

pub(super) fn emit_vec_len(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { len, .. } => {
            return store_slot(builder, proc, frame, dest, Value::Constant(len as i64))
        }
        Ty::Slice(_) => {
            let len = load_count_at(builder, frame, proc.slots[vec].offset + SLICE_LEN)?;
            return store_slot(builder, proc, frame, dest, len);
        }
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let empty = builder.new_label();
    let full = builder.new_label();
    let done = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), empty, full)
        .ir()?;
    builder.create_block(empty).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(0))?;
    builder.build_branch(done).ir()?;
    builder.create_block(full).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    store_slot(builder, proc, frame, dest, len)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn emit_vec_lit(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    elems: &[usize],
    elem: Ty,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let base = proc.slots[dest].offset;
    if elems.is_empty() {
        store_cell(builder, frame, base, Value::Constant(0))?;
        return store_cell(builder, frame, base + 1, Value::Constant(0));
    }
    let len = Value::Constant(elems.len() as i64);
    let obj = alloc_vec(builder, len.clone(), len, elem.cells())?;
    for (index, slot) in elems.iter().enumerate() {
        retain_managed(builder, proc, frame, *slot)?;
        let at = VEC_DATA + index as i64 * elem.cells();
        for cell in 0..elem.cells() {
            let value = load_cell(builder, frame, proc.slots[*slot].offset + cell, IrType::I16)?;
            store_object_cell(builder, &obj, at + cell, value)?;
        }
    }
    let header = gep(builder, frame, base)?;
    store_vec_ptr(builder, &header, &obj)
}

pub(super) fn emit_vec_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
    index: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { .. } => return emit_array_index(builder, proc, frame, dest, vec, index),
        Ty::Slice(_) => return emit_slice_index(builder, proc, frame, dest, vec, index, elem),
        Ty::String => return emit_string_index(builder, proc, frame, dest, vec, index),
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
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
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    copy_object_cells(
        builder,
        proc,
        frame,
        &obj,
        Value::Temp(data),
        dest,
        elem.cells(),
    )
}

pub(super) fn emit_vec_empty(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { len, .. } => {
            return store_slot(
                builder,
                proc,
                frame,
                dest,
                Value::Constant(i64::from(len == 0)),
            );
        }
        Ty::Slice(_) => {
            let len = load_count_at(builder, frame, proc.slots[vec].offset + SLICE_LEN)?;
            let empty = builder
                .build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1)
                .ir()?;
            return store_slot(builder, proc, frame, dest, Value::Temp(empty));
        }
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let yes = builder.new_label();
    let check = builder.new_label();
    let no = builder.new_label();
    let done = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), yes, check)
        .ir()?;
    builder.create_block(check).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let zero = builder
        .build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1)
        .ir()?;
    builder.build_branch_cond(Value::Temp(zero), yes, no).ir()?;
    builder.create_block(yes).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(1))?;
    builder.build_branch(done).ir()?;
    builder.create_block(no).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(0))?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn emit_vec_clear(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    offset: i64,
    elem: Ty,
) -> Result<(), CompileError> {
    let header = header_at(builder, proc, frame, vec, offset)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    release_element_buffer(builder, addr, bank, elem)?;
    store_cell(builder, &header, 0, Value::Constant(0))?;
    store_cell(builder, &header, 1, Value::Constant(0))
}

pub(super) fn emit_vec_index_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    index: usize,
    value: usize,
    elem: Ty,
    offset: i64,
    container: Ty,
) -> Result<(), CompileError> {
    if let Ty::Array { len, width, .. } = container {
        return emit_array_store_at(
            builder,
            proc,
            frame,
            proc.slots[vec].offset + offset,
            len,
            width,
            index,
            value,
        );
    }
    let place = if proc.slots[vec].ty.is_ref() {
        borrowed_place(builder, frame, proc.slots[vec].offset, offset)?
    } else {
        gep(builder, frame, proc.slots[vec].offset + offset)?
    };
    // `&Vec` already points at the header. A reference stored in a field needs
    // one more load to reach that header.
    let header = if container.is_ref() && !(proc.slots[vec].ty.is_ref() && offset == 0) {
        let addr = load_cell(builder, &place, 0, IrType::I16)?;
        let bank = load_cell(builder, &place, 1, IrType::I16)?;
        fat_from_parts(builder, addr, bank)?
    } else {
        place
    };
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bad = builder.new_label();
    let have = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), bad, have)
        .ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder
        .build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1)
        .ir()?;
    let write = builder.new_label();
    let detach = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(unique), write, detach)
        .ir()?;
    builder.create_block(detach).ir()?;
    detach_vec(builder, proc, &header, &obj, elem)?;
    builder.build_branch(write).ir()?;
    builder.create_block(write).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
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
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    let place = builder
        .build_pointer_offset(obj.clone(), Value::Temp(data), ptr_ty())
        .ir()?;
    release_managed_through(builder, proc, &place, elem)?;
    store_object_cells(
        builder,
        proc,
        frame,
        value,
        &obj,
        Value::Temp(data),
        elem.cells(),
    )
}

pub(super) fn store_immediate(
    builder: &mut IrBuilder,
    frame: &Value,
    dest: usize,
    proc: &Proc,
    tag: u16,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, Value::Constant(tag as i64))?;
    store_cell(builder, frame, base + 1, Value::Constant(0))?;
    store_cell(builder, frame, base + 2, Value::Constant(0))
}

pub(super) fn emit_vec_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { .. } => {
            return emit_array_get(
                builder, proc, frame, vec, index, value, dest, elem, some_tag, none_tag,
            )
        }
        Ty::Slice(_) => {
            return emit_slice_get(
                builder, proc, frame, vec, index, value, dest, elem, some_tag, none_tag,
            )
        }
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let none = builder.new_label();
    let have = builder.new_label();
    let done = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), none, have)
        .ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder
        .build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1)
        .ir()?;
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(in_range), ok, none)
        .ir()?;
    builder.create_block(ok).ir()?;
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    copy_object_cells(
        builder,
        proc,
        frame,
        &obj,
        Value::Temp(data),
        value,
        elem.cells(),
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

pub(super) fn emit_vec_pop(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    offset: i64,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let header = header_at(builder, proc, frame, vec, offset)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let none = builder.new_label();
    let have = builder.new_label();
    let done = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), none, have)
        .ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let empty = builder
        .build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1)
        .ir()?;
    let live = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(empty), none, live)
        .ir()?;
    builder.create_block(live).ir()?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder
        .build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1)
        .ir()?;
    let write = builder.new_label();
    let detach = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(unique), write, detach)
        .ir()?;
    builder.create_block(detach).ir()?;
    detach_vec(builder, proc, &header, &obj, elem)?;
    builder.build_branch(write).ir()?;
    builder.create_block(write).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let next = builder
        .build_binary(IrBinaryOp::Sub, len, Value::Constant(1), COUNT)
        .ir()?;
    store_count(builder, &obj, VEC_LEN, Value::Temp(next))?;
    let scaled = scale_index(builder, Value::Temp(next), elem.cells())?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    copy_object_cells(
        builder,
        proc,
        frame,
        &obj,
        Value::Temp(data),
        value,
        elem.cells(),
    )?;
    let place = builder
        .build_pointer_offset(obj.clone(), Value::Temp(data), ptr_ty())
        .ir()?;
    release_managed_through(builder, proc, &place, elem)?;
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

pub(super) fn emit_vec_push(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    offset: i64,
    value: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    let header = header_at(builder, proc, frame, vec, offset)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let fresh = builder.new_label();
    let have = builder.new_label();
    let append = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), fresh, have)
        .ir()?;

    builder.create_block(fresh).ir()?;
    let obj = alloc_vec(
        builder,
        Value::Constant(0),
        Value::Constant(4),
        elem.cells(),
    )?;
    store_vec_ptr(builder, &header, &obj)?;
    builder.build_branch(append).ir()?;

    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder
        .build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1)
        .ir()?;
    let detach = builder.new_label();
    let owned = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(unique), owned, detach)
        .ir()?;

    builder.create_block(detach).ir()?;
    detach_vec(builder, proc, &header, &obj, elem)?;
    builder.build_branch(append).ir()?;

    builder.create_block(owned).ir()?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let cap = load_count(builder, &obj, VEC_CAP)?;
    let full = builder
        .build_binary(IrBinaryOp::Uge, len, cap.clone(), IrType::I1)
        .ir()?;
    let grow = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(full), grow, append)
        .ir()?;
    builder.create_block(grow).ir()?;
    let new_cap = next_capacity(builder, cap)?;
    grow_vec(builder, &header, &obj, new_cap, elem.cells())?;
    builder.build_branch(append).ir()?;

    builder.create_block(append).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_count(builder, &obj, VEC_LEN)?;
    let scaled = scale_index(builder, len.clone(), elem.cells())?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    store_object_cells(
        builder,
        proc,
        frame,
        value,
        &obj,
        Value::Temp(data),
        elem.cells(),
    )?;
    let next = builder
        .build_binary(IrBinaryOp::Add, len, Value::Constant(1), COUNT)
        .ir()?;
    store_count(builder, &obj, VEC_LEN, Value::Temp(next))
}

pub(super) fn detach_vec(
    builder: &mut IrBuilder,
    proc: &Proc,
    header: &Value,
    obj: &Value,
    elem: Ty,
) -> Result<(), CompileError> {
    let cells = elem.cells();
    let len = load_count(builder, obj, VEC_LEN)?;
    let cap = load_count(builder, obj, VEC_CAP)?;
    let room = builder
        .build_binary(IrBinaryOp::Ugt, cap.clone(), len.clone(), IrType::I1)
        .ir()?;
    let use_cap = builder.new_label();
    let bump = builder.new_label();
    let join = builder.new_label();
    let saved = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_branch_cond(Value::Temp(room), use_cap, bump)
        .ir()?;
    builder.create_block(use_cap).ir()?;
    builder.build_store(cap, saved.clone(), COUNT).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(bump).ir()?;
    let bumped = next_capacity(builder, len.clone())?;
    builder.build_store(bumped, saved.clone(), COUNT).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(join).ir()?;
    let new_cap = Value::Temp(builder.build_load(saved, COUNT).ir()?);
    let new_obj = alloc_vec(builder, len.clone(), new_cap, cells)?;
    copy_elements(builder, obj, &new_obj, len.clone(), cells)?;
    retain_buffer_elements(builder, proc, &new_obj, len, elem)?;
    let refs = load_cell(builder, obj, VEC_REFS, IrType::I16)?;
    let next = builder
        .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
        .ir()?;
    store_object_cell(builder, obj, VEC_REFS, Value::Temp(next))?;
    store_vec_ptr(builder, header, &new_obj)
}

pub(super) fn grow_vec(
    builder: &mut IrBuilder,
    header: &Value,
    obj: &Value,
    new_cap: Value,
    cells: i64,
) -> Result<(), CompileError> {
    let len = load_count(builder, obj, VEC_LEN)?;
    let new_obj = alloc_vec(builder, len.clone(), new_cap, cells)?;
    copy_elements(builder, obj, &new_obj, len, cells)?;
    builder
        .build_call(
            Value::Global("free".to_string()),
            vec![obj.clone()],
            IrType::Void,
        )
        .ir()?;
    store_vec_ptr(builder, header, &new_obj)
}

pub(super) fn alloc_vec(
    builder: &mut IrBuilder,
    len: Value,
    cap: Value,
    cells: i64,
) -> Result<Value, CompileError> {
    // Check the allocation expression before multiplying or adding the header.
    // In particular, a bulk fill must not wrap a huge length into a tiny buffer.
    let cap = as_i32(builder, cap)?;
    if cells > 0 {
        let max_capacity = (i64::from(u32::MAX) - VEC_DATA) / cells;
        let overflow = builder
            .build_binary(
                IrBinaryOp::Ugt,
                cap.clone(),
                Value::Constant(max_capacity),
                IrType::I1,
            )
            .ir()?;
        let bad = builder.new_label();
        let ok = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(overflow), bad, ok)
            .ir()?;
        builder.create_block(bad).ir()?;
        runtime_fail(builder, FAIL_OVERFLOW)?;
        builder.create_block(ok).ir()?;
    }
    let scaled = scale_index(builder, cap.clone(), cells)?;
    let words = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
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
    store_count(builder, &obj, VEC_CAP, cap)?;
    Ok(obj)
}

pub(super) fn store_vec_ptr(
    builder: &mut IrBuilder,
    header: &Value,
    obj: &Value,
) -> Result<(), CompileError> {
    let addr = low16_of_ptr(builder, obj)?;
    let bank = bank16_of_ptr(builder, obj)?;
    store_cell(builder, header, 0, addr)?;
    store_cell(builder, header, 1, bank)
}

pub(super) fn scale_index(
    builder: &mut IrBuilder,
    index: Value,
    cells: i64,
) -> Result<Value, CompileError> {
    if cells == 1 {
        return Ok(index);
    }
    let scaled = builder
        .build_binary(IrBinaryOp::Mul, index, Value::Constant(cells), COUNT)
        .ir()?;
    Ok(Value::Temp(scaled))
}

pub(super) fn copy_elements(
    builder: &mut IrBuilder,
    src: &Value,
    dest: &Value,
    len: Value,
    cells: i64,
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
        .build_binary(IrBinaryOp::Ult, i.clone(), len.clone(), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let scaled = scale_index(builder, i.clone(), cells)?;
    let data = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    for cell in 0..cells {
        let at = builder
            .build_binary(
                IrBinaryOp::Add,
                Value::Temp(data),
                Value::Constant(cell),
                COUNT,
            )
            .ir()?;
        let from = builder
            .build_pointer_offset(src.clone(), Value::Temp(at), ptr_ty())
            .ir()?;
        let word = Value::Temp(builder.build_load(from, IrType::I16).ir()?);
        let into = builder
            .build_pointer_offset(dest.clone(), Value::Temp(at), ptr_ty())
            .ir()?;
        builder.build_store(word, into, IrType::I16).ir()?;
    }
    let next = builder
        .build_binary(IrBinaryOp::Add, i, Value::Constant(1), COUNT)
        .ir()?;
    builder.build_store(Value::Temp(next), i_slot, COUNT).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

pub(super) fn store_object_cells(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    src: usize,
    obj: &Value,
    data: Value,
    cells: i64,
) -> Result<(), CompileError> {
    retain_managed(builder, proc, frame, src)?;
    for cell in 0..cells {
        let value = load_cell(builder, frame, proc.slots[src].offset + cell, IrType::I16)?;
        let at = builder
            .build_binary(IrBinaryOp::Add, data.clone(), Value::Constant(cell), COUNT)
            .ir()?;
        let into = builder
            .build_pointer_offset(obj.clone(), Value::Temp(at), ptr_ty())
            .ir()?;
        builder.build_store(value, into, IrType::I16).ir()?;
    }
    Ok(())
}

pub(super) fn copy_object_cells(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    obj: &Value,
    data: Value,
    dest: usize,
    cells: i64,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    for cell in 0..cells {
        let at = builder
            .build_binary(IrBinaryOp::Add, data.clone(), Value::Constant(cell), COUNT)
            .ir()?;
        let from = builder
            .build_pointer_offset(obj.clone(), Value::Temp(at), ptr_ty())
            .ir()?;
        let word = Value::Temp(builder.build_load(from, IrType::I16).ir()?);
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    retain_managed(builder, proc, frame, dest)?;
    Ok(())
}

/// Double capacity, saturating at u32::MAX; fail when it cannot grow.
pub(super) fn next_capacity(builder: &mut IrBuilder, cap: Value) -> Result<Value, CompileError> {
    let slot = builder.build_alloca(COUNT, None).ir()?;
    let zero = builder.new_label();
    let scale = builder.new_label();
    let pick_max = builder.new_label();
    let pick_double = builder.new_label();
    let check = builder.new_label();
    let bad = builder.new_label();
    let done = builder.new_label();
    let is_zero = builder
        .build_binary(IrBinaryOp::Eq, cap.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_zero), zero, scale)
        .ir()?;
    builder.create_block(zero).ir()?;
    builder
        .build_store(Value::Constant(4), slot.clone(), COUNT)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(scale).ir()?;
    let doubled = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, cap.clone(), cap.clone(), COUNT)
            .ir()?,
    );
    let overflow = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ult, doubled.clone(), cap.clone(), IrType::I1)
            .ir()?,
    );
    builder
        .build_branch_cond(overflow, pick_max, pick_double)
        .ir()?;
    builder.create_block(pick_max).ir()?;
    builder
        .build_store(Value::Constant(0xffff_ffff), slot.clone(), COUNT)
        .ir()?;
    builder.build_branch(check).ir()?;
    builder.create_block(pick_double).ir()?;
    builder.build_store(doubled, slot.clone(), COUNT).ir()?;
    builder.build_branch(check).ir()?;
    builder.create_block(check).ir()?;
    let candidate = Value::Temp(builder.build_load(slot.clone(), COUNT).ir()?);
    let grew = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ugt, candidate, cap, IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(grew, done, bad).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_OVERFLOW)?;
    builder.create_block(done).ir()?;
    Ok(Value::Temp(builder.build_load(slot, COUNT).ir()?))
}
