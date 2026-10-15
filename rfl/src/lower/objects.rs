//! Objects support for RFL lowering.

use super::*;

pub(super) fn emit_record(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    fields: &[usize],
) -> Result<(), CompileError> {
    if keeps(proc, proc.slots[dest].ty) {
        release_tree(
            builder,
            proc,
            frame,
            proc.slots[dest].offset,
            proc.slots[dest].ty,
        )?;
    }
    let mut offset = proc.slots[dest].offset;
    for field in fields {
        let ty = proc.slots[*field].ty;
        for index in 0..ty.cells() {
            let value = load_cell(
                builder,
                frame,
                proc.slots[*field].offset + index,
                IrType::I16,
            )?;
            store_cell(builder, frame, offset, value)?;
            offset += 1;
        }
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, offset - ty.cells(), ty)?;
        }
    }
    Ok(())
}

pub(super) fn emit_take(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    offset: i64,
) -> Result<(), CompileError> {
    let ty = proc.slots[dest].ty;
    if proc.slots[src].ty.is_ref() {
        let place = borrowed_place(builder, frame, proc.slots[src].offset, offset)?;
        if keeps(proc, ty) {
            release_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
        }
        for index in 0..ty.cells() {
            let value = load_pointee_cell(builder, &place, index)?;
            store_cell(builder, frame, proc.slots[dest].offset + index, value)?;
        }
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
        }
        return Ok(());
    }
    let from = proc.slots[src].offset + offset;
    if keeps(proc, ty) {
        release_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
    }
    for index in 0..ty.cells() {
        let value = load_cell(builder, frame, from + index, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + index, value)?;
    }
    if keeps(proc, ty) {
        retain_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
    }
    Ok(())
}

pub(super) fn emit_field_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    offset: i64,
    src: usize,
) -> Result<(), CompileError> {
    let ty = proc.slots[src].ty;
    if proc.slots[dest].ty.is_ref() {
        let place = borrowed_place(builder, frame, proc.slots[dest].offset, offset)?;
        release_through(builder, proc, &place, ty)?;
        for index in 0..ty.cells() {
            let value = load_cell(builder, frame, proc.slots[src].offset + index, IrType::I16)?;
            store_pointee_cell(builder, &place, index, value)?;
        }
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, proc.slots[src].offset, ty)?;
        }
        return Ok(());
    }
    let at = proc.slots[dest].offset + offset;
    if keeps(proc, ty) {
        release_tree(builder, proc, frame, at, ty)?;
    }
    for index in 0..ty.cells() {
        let value = load_cell(builder, frame, proc.slots[src].offset + index, IrType::I16)?;
        store_cell(builder, frame, at + index, value)?;
    }
    if keeps(proc, ty) {
        retain_tree(builder, proc, frame, at, ty)?;
    }
    Ok(())
}

pub(super) fn emit_make(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    tag: u16,
    fields: &[(usize, i64)],
) -> Result<(), CompileError> {
    // Keep each field alive before `dest` is overwritten. `x = Cons(1, x)` retains
    // the old node, copies it into the new one, then drops the slot's own share.
    for (slot, _) in fields {
        if keeps(proc, proc.slots[*slot].ty) {
            retain_managed(builder, proc, frame, *slot)?;
        }
    }
    if fields.is_empty() {
        release_managed(builder, proc, frame, dest)?;
        let base = proc.slots[dest].offset;
        store_cell(builder, frame, base, Value::Constant(tag as i64))?;
        store_cell(builder, frame, base + 1, Value::Constant(0))?;
        return store_cell(builder, frame, base + 2, Value::Constant(0));
    }
    let words = 2 + fields.iter().map(|(_, cells)| *cells).sum::<i64>();
    let size = i32_const(builder, words)?;
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
    store_object_cell(builder, &obj, OBJ_REFS, Value::Constant(1))?;
    store_object_cell(builder, &obj, OBJ_TAG, Value::Constant(tag as i64))?;
    let mut offset = OBJ_FIELDS;
    for (slot, cells) in fields {
        let src = proc.slots[*slot].offset;
        for index in 0..*cells {
            let value = load_cell(builder, frame, src + index, IrType::I16)?;
            store_object_cell(builder, &obj, offset, value)?;
            offset += 1;
        }
    }
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    release_managed(builder, proc, frame, dest)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, Value::Constant(tag as i64))?;
    store_cell(builder, frame, base + 1, addr)?;
    store_cell(builder, frame, base + 2, bank)
}

pub(super) fn emit_read(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    offset: i64,
    cells: i64,
) -> Result<(), CompileError> {
    let src_at = proc.slots[src].offset;
    let addr = load_cell(builder, frame, src_at + 1, IrType::I16)?;
    let bank = load_cell(builder, frame, src_at + 2, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let mut words = Vec::new();
    for index in 0..cells {
        let ptr = gep(builder, &obj, offset + index)?;
        words.push(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?));
    }
    let ty = proc.slots[dest].ty;
    if ty.is_managed() {
        retain_words(builder, &words, ty)?;
        release_managed(builder, proc, frame, dest)?;
    } else if keeps(proc, ty) {
        release_managed(builder, proc, frame, dest)?;
    }
    let dest_at = proc.slots[dest].offset;
    for (index, value) in words.into_iter().enumerate() {
        store_cell(builder, frame, dest_at + index as i64, value)?;
    }
    if keeps(proc, ty) && !ty.is_managed() {
        retain_tree(builder, proc, frame, dest_at, ty)?;
    }
    Ok(())
}

pub(super) fn emit_try(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    src: usize,
    dest: usize,
    result: usize,
    ok_tag: u16,
    fail_tag: u16,
    payload: Ty,
    fail_slot: Option<usize>,
    fail: Option<Ty>,
    release: bool,
) -> Result<(), CompileError> {
    let tag = load_cell(builder, frame, proc.slots[src].offset, IrType::I16)?;
    let ok = builder.new_label();
    let fail_block = builder.new_label();
    let is_ok = builder
        .build_binary(
            IrBinaryOp::Eq,
            tag,
            Value::Constant(ok_tag as i64),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_ok), ok, fail_block)
        .ir()?;
    builder.create_block(fail_block).ir()?;
    if let Some(fail_ty) = fail {
        let slot = fail_slot.expect("a payload failure has a slot");
        emit_read(builder, proc, frame, slot, src, OBJ_FIELDS, fail_ty.cells())?;
        emit_make(
            builder,
            proc,
            frame,
            result,
            fail_tag,
            &[(slot, fail_ty.cells())],
        )?;
    } else {
        store_immediate(builder, frame, result, proc, fail_tag)?;
    }
    emit_return(builder, proc, frame, release)?;
    builder.create_block(ok).ir()?;
    emit_read(builder, proc, frame, dest, src, OBJ_FIELDS, payload.cells())
}
