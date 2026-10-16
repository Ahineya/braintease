//! Memory support for RFL lowering.

use super::*;

pub(super) fn ptr_is_null(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder
        .build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32)
        .ir()?;
    let zero = wide_zero(builder, IrType::I32)?;
    Ok(Value::Temp(
        builder
            .build_binary(IrBinaryOp::Eq, Value::Temp(bits), zero, IrType::I1)
            .ir()?,
    ))
}

pub(super) fn low16_of_ptr(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder
        .build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32)
        .ir()?;
    Ok(Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, Value::Temp(bits), IrType::I16)
            .ir()?,
    ))
}

pub(super) fn bank16_of_ptr(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder
        .build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32)
        .ir()?;
    let shifted = builder
        .build_binary(
            IrBinaryOp::LShr,
            Value::Temp(bits),
            Value::Constant(16),
            IrType::I32,
        )
        .ir()?;
    Ok(Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, Value::Temp(shifted), IrType::I16)
            .ir()?,
    ))
}

pub(super) fn as_i32(builder: &mut IrBuilder, value: Value) -> Result<Value, CompileError> {
    Ok(Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, value, Value::Constant(0), IrType::I32)
            .ir()?,
    ))
}

pub(super) fn store_object_cell(
    builder: &mut IrBuilder,
    obj: &Value,
    offset: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = gep(builder, obj, offset)?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

pub(super) fn load_count(
    builder: &mut IrBuilder,
    obj: &Value,
    offset: i64,
) -> Result<Value, CompileError> {
    let ptr = gep(builder, obj, offset)?;
    Ok(Value::Temp(builder.build_load(ptr, COUNT).ir()?))
}

pub(super) fn store_count(
    builder: &mut IrBuilder,
    obj: &Value,
    offset: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = gep(builder, obj, offset)?;
    builder.build_store(value, ptr, COUNT).ir()
}

pub(super) fn load_count_at(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
) -> Result<Value, CompileError> {
    load_cell(builder, frame, offset, COUNT)
}

pub(super) fn store_count_at(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = gep(builder, frame, offset)?;
    builder.build_store(value, ptr, COUNT).ir()
}

pub(super) fn fat_from_parts(
    builder: &mut IrBuilder,
    addr: Value,
    bank: Value,
) -> Result<Value, CompileError> {
    let low = builder
        .build_unary(IrUnaryOp::ZExt, addr, IrType::I32)
        .ir()?;
    let high = builder
        .build_unary(IrUnaryOp::ZExt, bank, IrType::I32)
        .ir()?;
    let shifted = builder
        .build_binary(
            IrBinaryOp::Shl,
            Value::Temp(high),
            Value::Constant(16),
            IrType::I32,
        )
        .ir()?;
    let bits = builder
        .build_binary(
            IrBinaryOp::Or,
            Value::Temp(low),
            Value::Temp(shifted),
            IrType::I32,
        )
        .ir()?;
    let raw = builder
        .build_unary(IrUnaryOp::IntToPtr, Value::Temp(bits), ptr_ty())
        .ir()?;
    Ok(fat_temp(raw, BankTag::Mixed))
}

pub(super) fn copy_slot(
    builder: &mut IrBuilder,
    src_proc: &Proc,
    src_frame: &Value,
    src: usize,
    dest_proc: &Proc,
    dest_frame: &Value,
    dest: usize,
) -> Result<(), CompileError> {
    let ty = src_proc.slots[src].ty;
    if ty == Ty::Unit {
        return Ok(());
    }
    if cell_wise(ty) {
        for index in 0..ty.cells() {
            let value = load_cell(
                builder,
                src_frame,
                src_proc.slots[src].offset + index,
                IrType::I16,
            )?;
            store_cell(
                builder,
                dest_frame,
                dest_proc.slots[dest].offset + index,
                value,
            )?;
        }
        if keeps(src_proc, ty) {
            retain_tree(
                builder,
                src_proc,
                dest_frame,
                dest_proc.slots[dest].offset,
                ty,
            )?;
        }
        return Ok(());
    }
    let value = load_slot(builder, src_proc, src_frame, src)?;
    store_slot(builder, dest_proc, dest_frame, dest, value)
}

pub(super) fn load_slot(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<Value, CompileError> {
    let info = &proc.slots[slot];
    if info.ty == Ty::Unit {
        return Ok(Value::Constant(0));
    }
    if info.shared {
        let obj = load_fat(builder, frame, info.offset)?;
        let at = builder
            .build_pointer_offset(obj, Value::Constant(1), ptr_ty())
            .ir()?;
        return Ok(Value::Temp(builder.build_load(at, ir_ty(info.ty)).ir()?));
    }
    load_cell(builder, frame, info.offset, ir_ty(info.ty))
}

pub(super) fn store_slot(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    value: Value,
) -> Result<(), CompileError> {
    let info = &proc.slots[slot];
    if info.ty == Ty::Unit {
        return Ok(());
    }
    let ptr = if info.shared {
        let obj = load_fat(builder, frame, info.offset)?;
        builder
            .build_pointer_offset(obj, Value::Constant(1), ptr_ty())
            .ir()?
    } else {
        gep(builder, frame, info.offset)?
    };
    builder.build_store(value, ptr, ir_ty(info.ty)).ir()
}

pub(super) fn load_cell(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
    ty: IrType,
) -> Result<Value, CompileError> {
    let ptr = gep(builder, frame, offset)?;
    Ok(Value::Temp(builder.build_load(ptr, ty).ir()?))
}

pub(super) fn store_cell(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = gep(builder, frame, offset)?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

pub(super) fn load_fat(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
) -> Result<Value, CompileError> {
    let ptr = gep(builder, frame, offset)?;
    let temp = builder.build_load(ptr, ptr_ty()).ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

pub(super) fn load_ptr_slot(builder: &mut IrBuilder, slot: &Value) -> Result<Value, CompileError> {
    let temp = builder.build_load(slot.clone(), ptr_ty()).ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

pub(super) fn gep(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
) -> Result<Value, CompileError> {
    builder
        .build_pointer_offset(frame.clone(), Value::Constant(offset), ptr_ty())
        .ir()
}

/// Pointer to cell `field` of the local borrowed by the reference at `ref_offset`.
pub(super) fn borrowed_place(
    builder: &mut IrBuilder,
    frame: &Value,
    ref_offset: i64,
    field: i64,
) -> Result<Value, CompileError> {
    let addr = load_cell(builder, frame, ref_offset, IrType::I16)?;
    let bank = load_cell(builder, frame, ref_offset + 1, IrType::I16)?;
    let base = fat_from_parts(builder, addr, bank)?;
    if field == 0 {
        Ok(base)
    } else {
        builder
            .build_pointer_offset(base, Value::Constant(field), ptr_ty())
            .ir()
    }
}

pub(super) fn load_pointee_cell(
    builder: &mut IrBuilder,
    place: &Value,
    index: i64,
) -> Result<Value, CompileError> {
    let ptr = builder
        .build_pointer_offset(place.clone(), Value::Constant(index), ptr_ty())
        .ir()?;
    Ok(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?))
}

pub(super) fn store_pointee_cell(
    builder: &mut IrBuilder,
    place: &Value,
    index: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = builder
        .build_pointer_offset(place.clone(), Value::Constant(index), ptr_ty())
        .ir()?;
    builder.build_store(value, ptr, IrType::I16).ir()?;
    Ok(())
}

pub(super) fn release_through(
    builder: &mut IrBuilder,
    proc: &Proc,
    place: &Value,
    ty: Ty,
) -> Result<(), CompileError> {
    release_managed_through(builder, proc, place, ty)
}

pub(super) fn header_at(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    offset: i64,
) -> Result<Value, CompileError> {
    if proc.slots[slot].ty.is_ref() {
        borrowed_place(builder, frame, proc.slots[slot].offset, offset)
    } else if offset == 0 {
        vec_header(builder, proc, frame, slot)
    } else {
        gep(builder, frame, proc.slots[slot].offset + offset)
    }
}

/// Pointer to the cells holding a vector's buffer address and bank.
pub(super) fn vec_header(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<Value, CompileError> {
    let offset = proc.slots[slot].offset;
    if proc.slots[slot].ty.is_ref() {
        let addr = load_cell(builder, frame, offset, IrType::I16)?;
        let bank = load_cell(builder, frame, offset + 1, IrType::I16)?;
        fat_from_parts(builder, addr, bank)
    } else {
        gep(builder, frame, offset)
    }
}

pub(super) fn emit_ptr_load(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    ptr: usize,
    index: Option<usize>,
    stride: i64,
    cells: i64,
) -> Result<(), CompileError> {
    let fat = pointer_at(builder, proc, frame, ptr, index, stride, true)?;
    let base = proc.slots[dest].offset;
    for cell in 0..cells {
        let at = builder
            .build_pointer_offset(fat.clone(), Value::Constant(cell), ptr_ty())
            .ir()?;
        let word = builder.build_load(at, IrType::I16).ir()?;
        store_cell(builder, frame, base + cell, Value::Temp(word))?;
    }
    Ok(())
}

pub(super) fn emit_ptr_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    ptr: usize,
    index: Option<usize>,
    src: usize,
    stride: i64,
    cells: i64,
) -> Result<(), CompileError> {
    let fat = pointer_at(builder, proc, frame, ptr, index, stride, true)?;
    let base = proc.slots[src].offset;
    for cell in 0..cells {
        let word = load_cell(builder, frame, base + cell, IrType::I16)?;
        let at = builder
            .build_pointer_offset(fat.clone(), Value::Constant(cell), ptr_ty())
            .ir()?;
        builder.build_store(word, at, IrType::I16).ir()?;
    }
    Ok(())
}

pub(super) fn emit_ptr_off(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    ptr: usize,
    index: usize,
    stride: i64,
) -> Result<(), CompileError> {
    let fat = pointer_at(builder, proc, frame, ptr, Some(index), stride, false)?;
    let addr = low16_of_ptr(builder, &fat)?;
    let bank = bank16_of_ptr(builder, &fat)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, addr)?;
    store_cell(builder, frame, base + 1, bank)
}

pub(super) fn pointer_at(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    ptr: usize,
    index: Option<usize>,
    stride: i64,
    fault_on_null: bool,
) -> Result<Value, CompileError> {
    let mut fat = load_fat(builder, frame, proc.slots[ptr].offset)?;
    if let Some(index) = index {
        let at = load_slot(builder, proc, frame, index)?;
        let scaled = if stride == 1 {
            at
        } else {
            Value::Temp(
                builder
                    .build_binary(IrBinaryOp::Mul, at, Value::Constant(stride), IrType::I16)
                    .ir()?,
            )
        };
        fat = builder.build_pointer_offset(fat, scaled, ptr_ty()).ir()?;
    }
    if fault_on_null {
        let addr = low16_of_ptr(builder, &fat)?;
        let bad = builder.new_label();
        let ok = builder.new_label();
        let is_null = builder
            .build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1)
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(is_null), bad, ok)
            .ir()?;
        builder.create_block(bad).ir()?;
        fail(builder, FAIL_NULL)?;
        builder.build_return(Some(null_ptr())).ir()?;
        builder.create_block(ok).ir()?;
    }
    Ok(fat)
}

pub(super) fn emit_box_new(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<(), CompileError> {
    let words = 1 + proc.slots[slot].ty.cells();
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
    store_object_cell(builder, &obj, 0, Value::Constant(1))?;
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    let base = proc.slots[slot].offset;
    store_cell(builder, frame, base, addr)?;
    store_cell(builder, frame, base + 1, bank)
}

pub(super) fn ptr_ty() -> IrType {
    IrType::FatPtr(Box::new(IrType::I16))
}

pub(super) fn fat_temp(temp: u32, bank: BankTag) -> Value {
    Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(temp)),
        bank,
    })
}

pub(super) fn null_ptr() -> Value {
    Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(0)),
        bank: BankTag::Null,
    })
}

pub(super) fn fn_symbol(name: &str) -> String {
    format!("rfl_fn_{name}")
}
