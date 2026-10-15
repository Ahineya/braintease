//! Frames support for RFL lowering.

use super::*;

pub(super) const BUMP_PREV: i64 = 0;
pub(super) const BUMP_CAP: i64 = 2;
pub(super) const BUMP_USED: i64 = 3;
pub(super) const BUMP_DATA: i64 = 4;
pub(super) const BUMP_CHUNK: i64 = 512;
const FRAME_PREFIX: i64 = 3;

pub(super) fn emit_frame_alloc(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_frame_alloc".to_string(), ptr_ty());
    builder.add_parameter(0, IrType::I32);
    let entry = builder.new_label();
    let have = builder.new_label();
    let fit = builder.new_label();
    let grow = builder.new_label();
    let use_words = builder.new_label();
    let use_chunk = builder.new_label();
    let alloc = builder.new_label();
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.create_block(entry).ir()?;
    let size_ok = builder.new_label();
    let size_bad = builder.new_label();
    let fits = builder
        .build_binary(
            IrBinaryOp::Ule,
            Value::Temp(0),
            Value::Constant(65535 - BUMP_DATA - FRAME_PREFIX),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(fits), size_ok, size_bad)
        .ir()?;
    builder.create_block(size_bad).ir()?;
    fail(builder, FAIL_OVERFLOW)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(size_ok).ir()?;
    let requested = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Temp(0),
            Value::Constant(FRAME_PREFIX),
            IrType::I32,
        )
        .ir()?;
    let words = Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, Value::Temp(requested), IrType::I16)
            .ir()?,
    );
    let cap_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let chunk = load_bump(builder)?;
    let missing = ptr_is_null(builder, &chunk)?;
    builder.build_branch_cond(missing, grow, have).ir()?;

    builder.create_block(have).ir()?;
    let used = load_cell(builder, &chunk, BUMP_USED, IrType::I16)?;
    let cap = load_cell(builder, &chunk, BUMP_CAP, IrType::I16)?;
    let sum = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, used.clone(), words.clone(), IrType::I16)
            .ir()?,
    );
    let overflow = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ult, sum.clone(), used.clone(), IrType::I1)
            .ir()?,
    );
    let check = builder.new_label();
    builder.build_branch_cond(overflow, grow, check).ir()?;
    builder.create_block(check).ir()?;
    let too_big = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Ugt, sum.clone(), cap, IrType::I1)
            .ir()?,
    );
    builder.build_branch_cond(too_big, grow, fit).ir()?;

    builder.create_block(fit).ir()?;
    let data = gep(builder, &chunk, BUMP_DATA)?;
    let prefix = builder
        .build_pointer_offset(data, used.clone(), ptr_ty())
        .ir()?;
    store_cell(builder, &chunk, BUMP_USED, sum)?;
    let ptr = frame_payload(builder, &prefix, &chunk, used)?;
    builder.build_return(Some(ptr)).ir()?;

    builder.create_block(grow).ir()?;
    let bigger = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Ugt,
                words.clone(),
                Value::Constant(BUMP_CHUNK),
                IrType::I1,
            )
            .ir()?,
    );
    builder
        .build_branch_cond(bigger, use_words, use_chunk)
        .ir()?;
    builder.create_block(use_words).ir()?;
    builder
        .build_store(words.clone(), cap_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(alloc).ir()?;
    builder.create_block(use_chunk).ir()?;
    builder
        .build_store(Value::Constant(BUMP_CHUNK), cap_slot.clone(), IrType::I16)
        .ir()?;
    builder.build_branch(alloc).ir()?;

    builder.create_block(alloc).ir()?;
    let prev = load_bump(builder)?;
    let cap_n = Value::Temp(builder.build_load(cap_slot, IrType::I16).ir()?);
    let total = Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::Add,
                cap_n.clone(),
                Value::Constant(BUMP_DATA),
                IrType::I16,
            )
            .ir()?,
    );
    let size = as_i32(builder, total)?;
    let raw = builder
        .build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty())
        .ir()?
        .unwrap();
    let fresh = fat_temp(raw, BankTag::Mixed);
    let is_null = ptr_is_null(builder, &fresh)?;
    builder.build_branch_cond(is_null, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_ALLOC)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let prev_slot = gep(builder, &fresh, BUMP_PREV)?;
    builder.build_store(prev, prev_slot, ptr_ty()).ir()?;
    store_cell(builder, &fresh, BUMP_CAP, cap_n)?;
    store_cell(builder, &fresh, BUMP_USED, words)?;
    store_bump(builder, fresh.clone())?;
    let data = gep(builder, &fresh, BUMP_DATA)?;
    let ptr = frame_payload(builder, &data, &fresh, Value::Constant(0))?;
    builder.build_return(Some(ptr)).ir()
}

pub(super) fn emit_frame_release(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_frame_release".to_string(), IrType::Void);
    builder.add_parameter(0, ptr_ty());
    let entry = builder.new_label();
    let head = builder.new_label();
    let pop = builder.new_label();
    let rewind = builder.new_label();
    builder.create_block(entry).ir()?;
    let frame = fat_temp(0, BankTag::Mixed);
    let prefix = gep(builder, &frame, -FRAME_PREFIX)?;
    let owner = load_fat(builder, &prefix, 0)?;
    let mark = load_cell(builder, &prefix, 2, IrType::I16)?;
    let owner_bits = builder
        .build_unary(IrUnaryOp::PtrToInt, owner.clone(), IrType::I32)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let chunk = load_bump(builder)?;
    let chunk_bits = builder
        .build_unary(IrUnaryOp::PtrToInt, chunk.clone(), IrType::I32)
        .ir()?;
    let same = builder
        .build_binary(
            IrBinaryOp::Eq,
            Value::Temp(chunk_bits),
            Value::Temp(owner_bits),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(same), rewind, pop)
        .ir()?;
    builder.create_block(pop).ir()?;
    let prev = load_fat(builder, &chunk, BUMP_PREV)?;
    builder
        .build_call(Value::Global("free".to_string()), vec![chunk], IrType::Void)
        .ir()?;
    store_bump(builder, prev)?;
    builder.build_branch(head).ir()?;
    builder.create_block(rewind).ir()?;
    store_cell(builder, &owner, BUMP_USED, mark)?;
    builder.build_return(None).ir()
}

/// A frame carries its allocation checkpoint. Address-range searches are
/// ambiguous when a chunk spans banks or an arena has empty trailing chunks.
fn frame_payload(
    builder: &mut IrBuilder,
    prefix: &Value,
    chunk: &Value,
    mark: Value,
) -> Result<Value, CompileError> {
    let owner = gep(builder, prefix, 0)?;
    builder.build_store(chunk.clone(), owner, ptr_ty()).ir()?;
    store_cell(builder, prefix, 2, mark)?;
    gep(builder, prefix, FRAME_PREFIX)
}

pub(super) fn load_bump(builder: &mut IrBuilder) -> Result<Value, CompileError> {
    let temp = builder
        .build_load(Value::Global("rfl_bump".to_string()), ptr_ty())
        .ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

pub(super) fn store_bump(builder: &mut IrBuilder, ptr: Value) -> Result<(), CompileError> {
    builder
        .build_store(ptr, Value::Global("rfl_bump".to_string()), ptr_ty())
        .ir()
}

pub(super) fn emit_return(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    release: bool,
) -> Result<(), CompileError> {
    // Only process entry and C wrappers create frames with a null caller.
    // Ordinary RFL functions always return into a suspended activation.
    if proc.name == "main" || proc.c_export {
        let addr = load_cell(builder, frame, CALLER, IrType::I16)?;
        let is_root = builder
            .build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1)
            .ir()?;
        let root = builder.new_label();
        let caller = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(is_root), root, caller)
            .ir()?;
        builder.create_block(root).ir()?;
        release_managed_slots(builder, proc, frame)?;
        builder.build_return(Some(null_ptr())).ir()?;
        builder.create_block(caller).ir()?;
    }
    let caller_ptr = load_fat(builder, frame, CALLER)?;
    if let Some(result) = proc.result_slot {
        let dest = load_cell(builder, frame, DEST, IrType::I16)?;
        let slot = builder
            .build_pointer_offset(caller_ptr.clone(), dest, ptr_ty())
            .ir()?;
        if cell_wise(proc.ret) {
            let start = proc.slots[result].offset;
            let mut words = Vec::new();
            for index in 0..proc.ret.cells() {
                words.push(load_cell(builder, frame, start + index, IrType::I16)?);
            }
            if keeps(proc, proc.ret) {
                release_managed_through(builder, proc, &slot, proc.ret)?;
            }
            for (index, value) in words.into_iter().enumerate() {
                let cell = builder
                    .build_pointer_offset(slot.clone(), Value::Constant(index as i64), ptr_ty())
                    .ir()?;
                builder.build_store(value, cell, IrType::I16).ir()?;
            }
            if keeps(proc, proc.ret) {
                retain_managed_through(builder, proc, &slot, proc.ret)?;
            }
        } else {
            let value = load_slot(builder, proc, frame, result)?;
            builder.build_store(value, slot, ir_ty(proc.ret)).ir()?;
        }
    }
    release_managed_slots(builder, proc, frame)?;
    if release {
        let saved = builder.build_alloca(ptr_ty(), None).ir()?;
        builder
            .build_store(caller_ptr, saved.clone(), ptr_ty())
            .ir()?;
        builder
            .build_call(
                Value::Global("rfl_frame_release".to_string()),
                vec![frame.clone()],
                IrType::Void,
            )
            .ir()?;
        let caller_ptr = load_ptr_slot(builder, &saved)?;
        builder.build_return(Some(caller_ptr)).ir()
    } else {
        builder.build_return(Some(caller_ptr)).ir()
    }
}

pub(super) fn emit_tail_call(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    callee: &str,
    args: &[usize],
) -> Result<(), CompileError> {
    let callee_proc = built.procs.iter().find(|proc| proc.name == callee).unwrap();
    let mut saved = Vec::new();
    for arg in args {
        let ty = proc.slots[*arg].ty;
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, proc.slots[*arg].offset, ty)?;
        }
        let mut words = Vec::new();
        if ty != Ty::Unit {
            if cell_wise(ty) {
                for index in 0..ty.cells() {
                    let value =
                        load_cell(builder, frame, proc.slots[*arg].offset + index, IrType::I16)?;
                    let mem = builder.build_alloca(IrType::I16, None).ir()?;
                    builder.build_store(value, mem.clone(), IrType::I16).ir()?;
                    words.push(mem);
                }
            } else {
                let value = load_slot(builder, proc, frame, *arg)?;
                let mem = builder.build_alloca(ir_ty(ty), None).ir()?;
                builder.build_store(value, mem.clone(), ir_ty(ty)).ir()?;
                words.push(mem);
            }
        }
        saved.push(words);
    }
    let caller_mem = builder.build_alloca(ptr_ty(), None).ir()?;
    let caller_ptr = load_fat(builder, frame, CALLER)?;
    builder
        .build_store(caller_ptr, caller_mem.clone(), ptr_ty())
        .ir()?;
    let dest_mem = builder.build_alloca(IrType::I16, None).ir()?;
    let dest_off = load_cell(builder, frame, DEST, IrType::I16)?;
    builder
        .build_store(dest_off, dest_mem.clone(), IrType::I16)
        .ir()?;
    release_managed_slots(builder, proc, frame)?;
    builder
        .build_call(
            Value::Global("rfl_frame_release".to_string()),
            vec![frame.clone()],
            IrType::Void,
        )
        .ir()?;
    let words = i32_const(builder, callee_proc.frame_cells)?;
    let raw = builder
        .build_call(
            Value::Global("rfl_frame_alloc".to_string()),
            vec![words],
            ptr_ty(),
        )
        .ir()?
        .unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(
        builder,
        &callee_frame,
        FUNC_ID,
        Value::Constant(callee_proc.func_id as i64),
    )?;
    let saved_caller = load_ptr_slot(builder, &caller_mem)?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder
        .build_store(saved_caller, caller_slot, ptr_ty())
        .ir()?;
    let saved_dest = Value::Temp(builder.build_load(dest_mem, IrType::I16).ir()?);
    store_cell(builder, &callee_frame, DEST, saved_dest)?;
    for (words, param) in saved.iter().zip(&callee_proc.param_slots) {
        let ty = callee_proc.slots[*param].ty;
        let base = callee_proc.slots[*param].offset;
        if cell_wise(ty) {
            for (index, mem) in words.iter().enumerate() {
                let value = Value::Temp(builder.build_load(mem.clone(), IrType::I16).ir()?);
                store_cell(builder, &callee_frame, base + index as i64, value)?;
            }
        } else if let Some(mem) = words.first() {
            let value = Value::Temp(builder.build_load(mem.clone(), ir_ty(ty)).ir()?);
            let ptr = gep(builder, &callee_frame, base)?;
            builder.build_store(value, ptr, ir_ty(ty)).ir()?;
        }
    }
    builder.build_return(Some(callee_frame)).ir()
}

pub(super) fn emit_call(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    callee: &str,
    args: &[usize],
    dest: usize,
    next: usize,
) -> Result<(), CompileError> {
    let callee_proc = built.procs.iter().find(|proc| proc.name == callee).unwrap();
    let words = i32_const(builder, callee_proc.frame_cells)?;
    let raw = builder
        .build_call(
            Value::Global("rfl_frame_alloc".to_string()),
            vec![words],
            ptr_ty(),
        )
        .ir()?
        .unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(
        builder,
        &callee_frame,
        FUNC_ID,
        Value::Constant(callee_proc.func_id as i64),
    )?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder
        .build_store(frame.clone(), caller_slot, ptr_ty())
        .ir()?;
    store_cell(
        builder,
        &callee_frame,
        DEST,
        Value::Constant(proc.slots[dest].offset),
    )?;
    for (arg, param) in args.iter().zip(&callee_proc.param_slots) {
        copy_slot(
            builder,
            proc,
            frame,
            *arg,
            callee_proc,
            &callee_frame,
            *param,
        )?;
    }
    store_cell(builder, frame, RESUME, Value::Constant(next as i64))?;
    builder.build_return(Some(callee_frame)).ir()
}

pub(super) fn emit_apply(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    args: &[usize],
    arg_tys: &[Ty],
    dest: usize,
    next: usize,
) -> Result<(), CompileError> {
    let base = proc.slots[slot].offset;
    let id = load_cell(builder, frame, base, IrType::I16)?;
    let words = load_cell(builder, frame, base + 1, IrType::I16)?;
    let size = as_i32(builder, words)?;
    let raw = builder
        .build_call(
            Value::Global("rfl_frame_alloc".to_string()),
            vec![size],
            ptr_ty(),
        )
        .ir()?
        .unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &callee_frame, FUNC_ID, id)?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder
        .build_store(frame.clone(), caller_slot, ptr_ty())
        .ir()?;
    store_cell(
        builder,
        &callee_frame,
        DEST,
        Value::Constant(proc.slots[dest].offset),
    )?;
    let mut offset = PAYLOAD + proc.slots[dest].ty.cells();
    for (arg, ty) in args.iter().zip(arg_tys) {
        copy_to_offset(builder, proc, frame, *arg, &callee_frame, offset)?;
        offset += ty.cells();
    }
    let env_addr = load_cell(builder, frame, base + 2, IrType::I16)?;
    let env_bank = load_cell(builder, frame, base + 3, IrType::I16)?;
    store_cell(builder, &callee_frame, offset, env_addr)?;
    store_cell(builder, &callee_frame, offset + 1, env_bank)?;
    store_cell(builder, frame, RESUME, Value::Constant(next as i64))?;
    builder.build_return(Some(callee_frame)).ir()
}

pub(super) fn copy_to_offset(
    builder: &mut IrBuilder,
    src_proc: &Proc,
    src_frame: &Value,
    src: usize,
    dest_frame: &Value,
    dest_offset: i64,
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
            store_cell(builder, dest_frame, dest_offset + index, value)?;
        }
        if keeps(src_proc, ty) {
            retain_tree(builder, src_proc, dest_frame, dest_offset, ty)?;
        }
        return Ok(());
    }
    let value = load_slot(builder, src_proc, src_frame, src)?;
    let ptr = gep(builder, dest_frame, dest_offset)?;
    builder.build_store(value, ptr, ir_ty(ty)).ir()
}

pub(super) fn emit_driver(builder: &mut IrBuilder, built: &Built) -> Result<(), CompileError> {
    let main = built.procs.iter().find(|proc| proc.name == "main").unwrap();
    builder.create_function("main".to_string(), IrType::I16);
    let entry = builder.new_label();
    let head = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let frame = builder
        .build_alloca(IrType::I16, Some(Value::Constant(main.frame_cells)))
        .ir()?;
    store_cell(builder, &frame, RESUME, Value::Constant(0))?;
    store_cell(
        builder,
        &frame,
        FUNC_ID,
        Value::Constant(main.func_id as i64),
    )?;
    let caller_slot = gep(builder, &frame, CALLER)?;
    builder
        .build_store(null_ptr(), caller_slot, ptr_ty())
        .ir()?;
    store_cell(builder, &frame, DEST, Value::Constant(0))?;
    let cur_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder
        .build_store(frame.clone(), cur_slot.clone(), ptr_ty())
        .ir()?;
    let next_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_branch(head).ir()?;

    builder.create_block(head).ir()?;
    let cur = load_ptr_slot(builder, &cur_slot)?;
    let id = load_cell(builder, &cur, FUNC_ID, IrType::I16)?;
    let merge = builder.new_label();
    emit_dispatch(
        builder,
        built,
        &cur,
        &next_slot,
        id,
        merge,
        Some(Value::Constant(1)),
    )?;
    builder.create_block(merge).ir()?;
    let next = load_ptr_slot(builder, &next_slot)?;
    let bits = builder
        .build_unary(IrUnaryOp::PtrToInt, next.clone(), IrType::I32)
        .ir()?;
    let is_null = builder
        .build_binary(
            IrBinaryOp::Eq,
            Value::Temp(bits),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    let again = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(is_null), done, again)
        .ir()?;
    builder.create_block(again).ir()?;
    builder.build_store(next, cur_slot, ptr_ty()).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    if main.ret == Ty::Unit {
        builder.build_return(Some(Value::Constant(0))).ir()
    } else {
        let value = load_cell(builder, &frame, PAYLOAD, IrType::I16)?;
        builder.build_return(Some(value)).ir()
    }
}

pub(super) fn emit_c_export(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
) -> Result<(), CompileError> {
    builder.create_function(proc.name.clone(), c_ret_ir(proc.ret, &built.repr_c));
    for (index, slot) in proc.param_slots.iter().enumerate() {
        builder.add_parameter(
            index as u32,
            c_param_ir(proc.slots[*slot].ty, &built.repr_c),
        );
    }
    let entry = builder.new_label();
    let head = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let frame = builder
        .build_alloca(IrType::I16, Some(Value::Constant(proc.frame_cells)))
        .ir()?;
    store_cell(builder, &frame, RESUME, Value::Constant(0))?;
    store_cell(
        builder,
        &frame,
        FUNC_ID,
        Value::Constant(proc.func_id as i64),
    )?;
    let caller_slot = gep(builder, &frame, CALLER)?;
    builder
        .build_store(null_ptr(), caller_slot, ptr_ty())
        .ir()?;
    store_cell(builder, &frame, DEST, Value::Constant(0))?;
    for (index, slot) in proc.param_slots.iter().enumerate() {
        let ty = proc.slots[*slot].ty;
        store_c_value(
            builder,
            &frame,
            proc.slots[*slot].offset,
            ty,
            Value::Temp(index as u32),
            &built.repr_c,
        )?;
    }
    let cur_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder
        .build_store(frame.clone(), cur_slot.clone(), ptr_ty())
        .ir()?;
    let next_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_branch(head).ir()?;

    builder.create_block(head).ir()?;
    let cur = load_ptr_slot(builder, &cur_slot)?;
    let id = load_cell(builder, &cur, FUNC_ID, IrType::I16)?;
    let merge = builder.new_label();
    let miss = if proc.ret == Ty::Unit {
        None
    } else {
        Some(Value::Constant(0))
    };
    emit_dispatch(builder, built, &cur, &next_slot, id, merge, miss)?;
    builder.create_block(merge).ir()?;
    let next = load_ptr_slot(builder, &next_slot)?;
    let bits = builder
        .build_unary(IrUnaryOp::PtrToInt, next.clone(), IrType::I32)
        .ir()?;
    let is_null = builder
        .build_binary(
            IrBinaryOp::Eq,
            Value::Temp(bits),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    let again = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(is_null), done, again)
        .ir()?;
    builder.create_block(again).ir()?;
    builder.build_store(next, cur_slot, ptr_ty()).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    if proc.ret == Ty::Unit {
        builder.build_return(None).ir()
    } else if repr_struct(&built.repr_c, proc.ret) {
        let ptr = gep(builder, &frame, PAYLOAD)?;
        builder.build_return(Some(ptr)).ir()
    } else if matches!(proc.ret, Ty::Ptr(_) | Ty::CFn(_)) {
        let value = load_fat(builder, &frame, PAYLOAD)?;
        builder.build_return(Some(value)).ir()
    } else {
        let value = load_cell(builder, &frame, PAYLOAD, ir_ty(proc.ret))?;
        builder.build_return(Some(value)).ir()
    }
}

pub(super) fn store_c_value(
    builder: &mut IrBuilder,
    frame: &Value,
    offset: i64,
    ty: Ty,
    value: Value,
    repr: &[bool],
) -> Result<(), CompileError> {
    if ty == Ty::Unit {
        return Ok(());
    }
    if repr_struct(repr, ty) {
        let src = match value {
            Value::Temp(id) => fat_temp(id, BankTag::Mixed),
            other => other,
        };
        return copy_words(builder, &src, frame, offset, ty.cells());
    }
    let ptr = gep(builder, frame, offset)?;
    let ir = if matches!(ty, Ty::Ptr(_) | Ty::CFn(_)) {
        ptr_ty()
    } else {
        ir_ty(ty)
    };
    builder.build_store(value, ptr, ir).ir()
}

pub(super) fn copy_words(
    builder: &mut IrBuilder,
    src: &Value,
    frame: &Value,
    offset: i64,
    cells: i64,
) -> Result<(), CompileError> {
    for index in 0..cells {
        let ptr = builder
            .build_pointer_offset(src.clone(), Value::Constant(index), ptr_ty())
            .ir()?;
        let word = builder.build_load(ptr, IrType::I16).ir()?;
        store_cell(builder, frame, offset + index, Value::Temp(word))?;
    }
    Ok(())
}

pub(super) fn repr_struct(repr: &[bool], ty: Ty) -> bool {
    matches!(ty, Ty::Struct { id, .. } if repr.get(id as usize).copied().unwrap_or(false))
}

pub(super) fn emit_dispatch(
    builder: &mut IrBuilder,
    built: &Built,
    cur: &Value,
    next_slot: &Value,
    id: Value,
    merge: u32,
    on_miss: Option<Value>,
) -> Result<(), CompileError> {
    let entries: Vec<_> = built
        .procs
        .iter()
        .map(|proc| (proc.func_id as i64, builder.new_label()))
        .collect();
    let bad = builder.new_label();
    index_switch(builder, id, &entries, bad)?;
    for (proc, (_, label)) in built.procs.iter().zip(&entries) {
        builder.create_block(*label).ir()?;
        let raw = builder
            .build_call(
                Value::Global(fn_symbol(&proc.name)),
                vec![cur.clone()],
                ptr_ty(),
            )
            .ir()?
            .unwrap();
        builder
            .build_store(fat_temp(raw, BankTag::Mixed), next_slot.clone(), ptr_ty())
            .ir()?;
        builder.build_branch(merge).ir()?;
    }
    builder.create_block(bad).ir()?;
    if built.need_fail {
        fail(builder, FAIL_UNREACHABLE)?;
    }
    builder.build_return(on_miss).ir()
}
