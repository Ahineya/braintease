//! Ownership support for RFL lowering.

use super::*;

pub(super) fn keeps(proc: &Proc, ty: Ty) -> bool {
    match ty {
        Ty::Struct { id, .. } => proc.structs[id as usize]
            .iter()
            .any(|field| keeps(proc, *field)),
        Ty::Tuple { id, .. } => proc.tuples[id as usize]
            .iter()
            .any(|field| keeps(proc, *field)),
        Ty::Array { id, .. } => keeps(proc, proc.collections.arrays[id as usize]),
        other => other.is_managed(),
    }
}

pub(super) fn retain_tree(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    offset: i64,
    ty: Ty,
) -> Result<(), CompileError> {
    if !keeps(proc, ty) {
        return Ok(());
    }
    let place = gep(builder, frame, offset)?;
    retain_managed_through(builder, proc, &place, ty)
}

pub(super) fn release_tree(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    offset: i64,
    ty: Ty,
) -> Result<(), CompileError> {
    if !keeps(proc, ty) {
        return Ok(());
    }
    let place = gep(builder, frame, offset)?;
    release_managed_through(builder, proc, &place, ty)
}

pub(super) fn zero_managed_slots(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    for (index, slot) in proc.slots.iter().enumerate() {
        if (slot.shared || keeps(proc, slot.ty)) && !proc.param_slots.contains(&index) {
            let cells = if slot.shared { 2 } else { slot.ty.cells() };
            for cell in 0..cells {
                store_cell(builder, frame, slot.offset + cell, Value::Constant(0))?;
            }
        }
    }
    Ok(())
}

pub(super) fn release_managed_slots(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    if has_managed_slots(proc) {
        builder
            .build_call(
                Value::Global(format!("rfl_drop_frame_{}", proc.func_id)),
                vec![frame.clone()],
                IrType::Void,
            )
            .ir()?;
    }
    Ok(())
}

fn has_managed_slots(proc: &Proc) -> bool {
    proc.slots
        .iter()
        .any(|slot| slot.shared || keeps(proc, slot.ty))
}

/// Share cleanup across all returns and tail calls from a procedure. Expanding
/// every drop at each exit can make a small source function exceed a code bank.
pub(super) fn emit_frame_drops(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    for proc in &built.procs {
        if !has_managed_slots(proc) {
            continue;
        }
        builder.create_function(format!("rfl_drop_frame_{}", proc.func_id), IrType::Void);
        builder.add_parameter(0, ptr_ty());
        let entry = builder.new_label();
        builder.create_block(entry).ir()?;
        release_slots_body(builder, proc, &fat_temp(0, BankTag::Mixed))?;
        builder.build_return(None).ir()?;
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}

fn release_slots_body(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    let indices: Vec<_> = if proc.async_result.is_some() {
        (0..proc.slots.len()).rev().collect()
    } else {
        (0..proc.slots.len()).collect()
    };
    for index in indices {
        if proc.slots[index].shared {
            let addr = load_cell(builder, frame, proc.slots[index].offset, IrType::I16)?;
            let bank = load_cell(builder, frame, proc.slots[index].offset + 1, IrType::I16)?;
            adjust_refcount(builder, addr, bank, false)?;
        } else if keeps(proc, proc.slots[index].ty) {
            release_managed(builder, proc, frame, index)?;
        }
    }
    Ok(())
}

pub(super) fn retain_vec(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
) -> Result<(), CompileError> {
    let addr = load_cell(builder, frame, offset, IrType::I16)?;
    let bank = load_cell(builder, frame, offset + 1, IrType::I16)?;
    adjust_refcount(builder, addr, bank, true)
}

/// `retain` increments. Otherwise the last reference frees the buffer and any earlier one decrements.
pub(super) fn adjust_refcount(
    builder: &mut IrBuilder,
    addr: Value,
    bank: Value,
    retain: bool,
) -> Result<(), CompileError> {
    builder
        .build_call(
            Value::Global(
                if retain {
                    "rfl_retain"
                } else {
                    "rfl_release_box"
                }
                .into(),
            ),
            vec![addr, bank],
            IrType::Void,
        )
        .ir()?;
    Ok(())
}

pub(super) fn emit_retain_runtime(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    if !built.need_heap {
        return Ok(());
    }
    builder.create_function("rfl_retain".into(), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    adjust_refcount_body(builder, Value::Temp(0), Value::Temp(1), true)?;
    builder.build_return(None).ir()?;
    module.add_function(builder.finish_function().unwrap());
    builder.create_function("rfl_release_box".into(), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    adjust_refcount_body(builder, Value::Temp(0), Value::Temp(1), false)?;
    builder.build_return(None).ir()?;
    module.add_function(builder.finish_function().unwrap());
    Ok(())
}

fn adjust_refcount_body(
    builder: &mut IrBuilder,
    addr: Value,
    bank: Value,
    retain: bool,
) -> Result<(), CompileError> {
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), skip, go)
        .ir()?;
    builder.create_block(go).ir()?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    if retain {
        let overflow = builder
            .build_binary(
                IrBinaryOp::Eq,
                refs.clone(),
                Value::Constant(u16::MAX as i64),
                IrType::I1,
            )
            .ir()?;
        let bad = builder.new_label();
        let room = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(overflow), bad, room)
            .ir()?;
        builder.create_block(bad).ir()?;
        fail(builder, FAIL_OVERFLOW)?;
        builder.build_branch(skip).ir()?;
        builder.create_block(room).ir()?;
        let next = builder
            .build_binary(IrBinaryOp::Add, refs, Value::Constant(1), IrType::I16)
            .ir()?;
        store_object_cell(builder, &obj, VEC_REFS, Value::Temp(next))?;
        builder.build_branch(skip).ir()?;
    } else {
        let last = builder
            .build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1)
            .ir()?;
        let free_it = builder.new_label();
        let dec = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(last), free_it, dec)
            .ir()?;
        builder.create_block(dec).ir()?;
        let next = builder
            .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
            .ir()?;
        store_object_cell(builder, &obj, VEC_REFS, Value::Temp(next))?;
        builder.build_branch(skip).ir()?;
        builder.create_block(free_it).ir()?;
        builder
            .build_call(Value::Global("free".to_string()), vec![obj], IrType::Void)
            .ir()?;
        builder.build_branch(skip).ir()?;
    }
    builder.create_block(skip).ir()?;
    Ok(())
}

pub(super) fn emit_vec_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    transfer: bool,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let cells = proc.slots[src].ty.cells();
    for index in 0..cells {
        let value = load_cell(builder, frame, proc.slots[src].offset + index, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + index, value)?;
    }
    if transfer {
        store_cell(builder, frame, proc.slots[src].offset, Value::Constant(0))?;
        store_cell(
            builder,
            frame,
            proc.slots[src].offset + 1,
            Value::Constant(0),
        )
    } else {
        retain_vec(builder, frame, proc.slots[dest].offset)
    }
}

pub(super) fn retain_managed(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<(), CompileError> {
    let offset = proc.slots[slot].offset;
    retain_tree(builder, proc, frame, offset, proc.slots[slot].ty)
}

pub(super) fn retain_words(
    builder: &mut IrBuilder,
    words: &[Value],
    ty: Ty,
) -> Result<(), CompileError> {
    let index = match ty {
        Ty::Vec(_)
        | Ty::Slice(_)
        | Ty::String
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::SharedOwner => 0,
        Ty::Adt(_) => 1,
        Ty::Func(_) => 2,
        _ => return Ok(()),
    };
    adjust_refcount(
        builder,
        words[index].clone(),
        words[index + 1].clone(),
        true,
    )
}

pub(super) fn load_through(
    builder: &mut IrBuilder,
    slot: &Value,
    index: i64,
) -> Result<Value, CompileError> {
    let ptr = builder
        .build_pointer_offset(slot.clone(), Value::Constant(index), ptr_ty())
        .ir()?;
    Ok(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?))
}

pub(super) fn store_through(
    builder: &mut IrBuilder,
    slot: &Value,
    index: i64,
    value: Value,
) -> Result<(), CompileError> {
    let ptr = builder
        .build_pointer_offset(slot.clone(), Value::Constant(index), ptr_ty())
        .ir()?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

pub(super) fn retain_managed_through(
    builder: &mut IrBuilder,
    proc: &Proc,
    slot: &Value,
    ty: Ty,
) -> Result<(), CompileError> {
    match ty {
        Ty::Array { id, len, width } => {
            let elem = proc.collections.arrays[id as usize];
            if !keeps(proc, elem) {
                return Ok(());
            }
            for i in 0..len {
                let place = gep(builder, slot, i64::from(i) * i64::from(width))?;
                retain_managed_through(builder, proc, &place, elem)?;
            }
            Ok(())
        }
        Ty::Struct { id, .. } => {
            through_fields(builder, proc, slot, &proc.structs[id as usize], true)
        }
        Ty::Tuple { id, .. } => {
            through_fields(builder, proc, slot, &proc.tuples[id as usize], true)
        }
        Ty::Vec(_)
        | Ty::Slice(_)
        | Ty::String
        | Ty::Adt(_)
        | Ty::Func(_)
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::SharedOwner => {
            let index = match ty {
                Ty::Vec(_)
                | Ty::Slice(_)
                | Ty::String
                | Ty::Owned(_)
                | Ty::ErasedOwner
                | Ty::SharedOwner => 0,
                Ty::Adt(_) => 1,
                _ => 2,
            };
            let addr = load_through(builder, slot, index)?;
            let bank = load_through(builder, slot, index + 1)?;
            adjust_refcount(builder, addr, bank, true)
        }
        _ => Ok(()),
    }
}

pub(super) fn through_fields(
    builder: &mut IrBuilder,
    proc: &Proc,
    slot: &Value,
    fields: &[Ty],
    retain: bool,
) -> Result<(), CompileError> {
    let mut at = 0i64;
    for field in fields {
        let ptr = builder
            .build_pointer_offset(slot.clone(), Value::Constant(at), ptr_ty())
            .ir()?;
        if retain {
            retain_managed_through(builder, proc, &ptr, *field)?;
        } else {
            release_managed_through(builder, proc, &ptr, *field)?;
        }
        at += field.cells();
    }
    Ok(())
}

pub(super) fn release_managed(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
) -> Result<(), CompileError> {
    let ty = proc.slots[slot].ty;
    let offset = proc.slots[slot].offset;
    release_tree(builder, proc, frame, offset, ty)
}

pub(super) fn release_managed_through(
    builder: &mut IrBuilder,
    proc: &Proc,
    slot: &Value,
    ty: Ty,
) -> Result<(), CompileError> {
    if let Some(name) = release_helper_name(ty) {
        builder
            .build_call(Value::Global(name), vec![slot.clone()], IrType::Void)
            .ir()?;
        return Ok(());
    }
    release_managed_body(builder, proc, slot, ty)
}

pub(super) fn release_managed_body(
    builder: &mut IrBuilder,
    proc: &Proc,
    slot: &Value,
    ty: Ty,
) -> Result<(), CompileError> {
    match ty {
        Ty::Owned(_) | Ty::ErasedOwner | Ty::SharedOwner => {
            let addr = load_through(builder, slot, 0)?;
            let bank = load_through(builder, slot, 1)?;
            // Clear first: a synchronous finalizer may perform nested cleanup.
            store_through(builder, slot, 0, Value::Constant(0))?;
            store_through(builder, slot, 1, Value::Constant(0))?;
            let skip = builder.new_label();
            let go = builder.new_label();
            let empty = builder
                .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
                .ir()?;
            builder
                .build_branch_cond(Value::Temp(empty), skip, go)
                .ir()?;
            builder.create_block(go).ir()?;
            builder
                .build_call(
                    Value::Global(match ty {
                        Ty::Owned(id) => format!("rfl_drop_owner_{id}"),
                        _ => "rfl_drop_erased_owner".into(),
                    }),
                    vec![addr, bank],
                    IrType::Void,
                )
                .ir()?;
            builder.build_branch(skip).ir()?;
            builder.create_block(skip).ir()?;
            Ok(())
        }
        Ty::Array { id, len, width } => {
            let elem = proc.collections.arrays[id as usize];
            if !keeps(proc, elem) {
                return Ok(());
            }
            for i in 0..len {
                let place = gep(builder, slot, i64::from(i) * i64::from(width))?;
                release_managed_through(builder, proc, &place, elem)?;
            }
            Ok(())
        }
        Ty::Struct { id, .. } => {
            through_fields(builder, proc, slot, &proc.structs[id as usize], false)
        }
        Ty::Tuple { id, .. } => {
            through_fields(builder, proc, slot, &proc.tuples[id as usize], false)
        }
        Ty::Vec(_) | Ty::Slice(_) | Ty::String => {
            let addr = load_through(builder, slot, 0)?;
            let bank = load_through(builder, slot, 1)?;
            release_buffer(builder, &proc.collections, addr, bank, ty)?;
            store_through(builder, slot, 0, Value::Constant(0))?;
            store_through(builder, slot, 1, Value::Constant(0))
        }
        Ty::Adt(id) => {
            let tag = load_through(builder, slot, 0)?;
            let addr = load_through(builder, slot, 1)?;
            let bank = load_through(builder, slot, 2)?;
            call_if_live(
                builder,
                &format!("rfl_drop_adt_{id}"),
                vec![tag, addr, bank],
            )?;
            store_through(builder, slot, 1, Value::Constant(0))?;
            store_through(builder, slot, 2, Value::Constant(0))
        }
        Ty::Func(_) => {
            let id = load_through(builder, slot, 0)?;
            let addr = load_through(builder, slot, 2)?;
            let bank = load_through(builder, slot, 3)?;
            call_if_live(builder, "rfl_drop_func", vec![id, addr, bank])?;
            store_through(builder, slot, 2, Value::Constant(0))?;
            store_through(builder, slot, 3, Value::Constant(0))
        }
        _ => Ok(()),
    }
}

/// `args[1]` is the heap address. A zero address is not an object.
pub(super) fn call_if_live(
    builder: &mut IrBuilder,
    name: &str,
    args: Vec<Value>,
) -> Result<(), CompileError> {
    let addr = args[1].clone();
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), skip, go)
        .ir()?;
    builder.create_block(go).ir()?;
    builder
        .build_call(Value::Global(name.to_string()), args, IrType::Void)
        .ir()?;
    builder.build_branch(skip).ir()?;
    builder.create_block(skip).ir()?;
    Ok(())
}

pub(super) fn emit_arc(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    // Types from checked but pruned functions do not require a runtime.
    if !built.need_heap {
        return Ok(());
    }
    for (id, adt) in built.adts.iter().enumerate() {
        emit_drop_adt(builder, id as u32, adt, built)?;
        module.add_function(builder.finish_function().unwrap());
    }
    if built.need_heap {
        emit_drop_func(builder, built)?;
        module.add_function(builder.finish_function().unwrap());
    }
    emit_buffer_drops(builder, module, built)?;
    Ok(())
}

pub(super) fn emit_drop_adt(
    builder: &mut IrBuilder,
    adt_id: u32,
    adt: &AdtDef,
    built: &Built,
) -> Result<(), CompileError> {
    builder.create_function(format!("rfl_drop_adt_{adt_id}"), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    builder.add_parameter(2, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let addr_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let bank_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let tag_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder
        .build_store(Value::Temp(1), addr_slot.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Temp(2), bank_slot.clone(), IrType::I16)
        .ir()?;
    builder
        .build_store(Value::Temp(0), tag_slot.clone(), IrType::I16)
        .ir()?;
    let head = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let addr = Value::Temp(builder.build_load(addr_slot.clone(), IrType::I16).ir()?);
    let is_null = builder
        .build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1)
        .ir()?;
    let live = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(is_null), done, live)
        .ir()?;
    builder.create_block(live).ir()?;
    let bank = Value::Temp(builder.build_load(bank_slot.clone(), IrType::I16).ir()?);
    let tag = Value::Temp(builder.build_load(tag_slot.clone(), IrType::I16).ir()?);
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, OBJ_REFS, IrType::I16)?;
    let last = builder
        .build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1)
        .ir()?;
    let drop_it = builder.new_label();
    let dec = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(last), drop_it, dec)
        .ir()?;
    builder.create_block(dec).ir()?;
    let next = builder
        .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
        .ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Temp(next))?;
    builder.build_branch(done).ir()?;
    builder.create_block(drop_it).ir()?;
    for (tag_index, ctor) in adt.ctors.iter().enumerate() {
        let matched = builder.new_label();
        let next_check = builder.new_label();
        let is_tag = builder
            .build_binary(
                IrBinaryOp::Eq,
                tag.clone(),
                Value::Constant(tag_index as i64),
                IrType::I1,
            )
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(is_tag), matched, next_check)
            .ir()?;
        builder.create_block(matched).ir()?;
        emit_drop_ctor(
            builder, built, &obj, ctor, adt_id, &addr_slot, &bank_slot, &tag_slot, head, done,
        )?;
        builder.create_block(next_check).ir()?;
    }
    builder
        .build_call(Value::Global("free".to_string()), vec![obj], IrType::Void)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

pub(super) fn emit_drop_ctor(
    builder: &mut IrBuilder,
    built: &Built,
    obj: &Value,
    ctor: &CtorDef,
    adt_id: u32,
    addr_slot: &Value,
    bank_slot: &Value,
    tag_slot: &Value,
    head: u32,
    done: u32,
) -> Result<(), CompileError> {
    let tail_at = ctor
        .fields
        .iter()
        .rposition(|field| matches!(field, Ty::Adt(id) if *id == adt_id));
    let mut offset = OBJ_FIELDS;
    let mut tail_tag = None;
    let mut tail_addr = None;
    let mut tail_bank = None;
    for (index, field) in ctor.fields.iter().enumerate() {
        if Some(index) == tail_at {
            tail_tag = Some(load_cell(builder, obj, offset, IrType::I16)?);
            tail_addr = Some(load_cell(builder, obj, offset + 1, IrType::I16)?);
            tail_bank = Some(load_cell(builder, obj, offset + 2, IrType::I16)?);
        } else {
            release_object_field(builder, built, obj, offset, *field)?;
        }
        offset += field.cells();
    }
    builder
        .build_call(
            Value::Global("free".to_string()),
            vec![obj.clone()],
            IrType::Void,
        )
        .ir()?;
    if let (Some(tag), Some(addr), Some(bank)) = (tail_tag, tail_addr, tail_bank) {
        builder
            .build_store(addr, addr_slot.clone(), IrType::I16)
            .ir()?;
        builder
            .build_store(bank, bank_slot.clone(), IrType::I16)
            .ir()?;
        builder
            .build_store(tag, tag_slot.clone(), IrType::I16)
            .ir()?;
        builder.build_branch(head).ir()
    } else {
        builder.build_branch(done).ir()
    }
}

pub(super) fn release_object_field(
    builder: &mut IrBuilder,
    built: &Built,
    obj: &Value,
    offset: i64,
    ty: Ty,
) -> Result<(), CompileError> {
    let proc = built.procs.first().expect("main exists");
    if !keeps(proc, ty) {
        return Ok(());
    }
    let place = gep(builder, obj, offset)?;
    release_managed_through(builder, proc, &place, ty)
}

pub(super) fn emit_drop_func(builder: &mut IrBuilder, built: &Built) -> Result<(), CompileError> {
    builder.create_function("rfl_drop_func".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    builder.add_parameter(2, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder
        .build_binary(
            IrBinaryOp::Eq,
            Value::Temp(1),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(is_null), skip, go)
        .ir()?;
    builder.create_block(go).ir()?;
    let obj = fat_from_parts(builder, Value::Temp(1), Value::Temp(2))?;
    let refs = load_cell(builder, &obj, OBJ_REFS, IrType::I16)?;
    let last = builder
        .build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1)
        .ir()?;
    let drop_it = builder.new_label();
    let dec = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(last), drop_it, dec)
        .ir()?;
    builder.create_block(dec).ir()?;
    let next = builder
        .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
        .ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Temp(next))?;
    builder.build_branch(skip).ir()?;
    builder.create_block(drop_it).ir()?;
    let capturing: Vec<&Proc> = built
        .procs
        .iter()
        .filter(|proc| !proc.captures.is_empty())
        .collect();
    let entries: Vec<_> = capturing
        .iter()
        .map(|proc| (proc.func_id as i64, builder.new_label()))
        .collect();
    let unknown = builder.new_label();
    index_switch(builder, Value::Temp(0), &entries, unknown)?;
    for (proc, (_, matched)) in capturing.iter().zip(&entries) {
        builder.create_block(*matched).ir()?;
        let captures: Vec<_> = if proc.captures.iter().any(|cap| cap.transfer) {
            proc.captures.iter().rev().collect()
        } else {
            proc.captures.iter().collect()
        };
        for cap in captures {
            if cap.shared {
                let addr = load_cell(builder, &obj, cap.offset, IrType::I16)?;
                let bank = load_cell(builder, &obj, cap.offset + 1, IrType::I16)?;
                adjust_refcount(builder, addr, bank, false)?;
            } else {
                release_object_field(builder, built, &obj, cap.offset, cap.ty)?;
            }
        }
        builder
            .build_call(
                Value::Global("free".to_string()),
                vec![obj.clone()],
                IrType::Void,
            )
            .ir()?;
        builder.build_branch(skip).ir()?;
    }
    builder.create_block(unknown).ir()?;
    builder
        .build_call(Value::Global("free".to_string()), vec![obj], IrType::Void)
        .ir()?;
    builder.build_branch(skip).ir()?;
    builder.create_block(skip).ir()?;
    builder.build_return(None).ir()
}
