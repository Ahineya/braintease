//! Owned payloads have stable addresses. Compiler temporaries may briefly retain
//! their allocation, but source programs cannot duplicate the resource owner.
use super::*;
pub(super) const OWNER_CLEANUP: i64 = 1;
pub(super) const OWNER_DATA: i64 = 5;

pub(super) fn transfer_slot(
    builder: &mut IrBuilder,
    src_proc: &Proc,
    src_frame: &Value,
    src: usize,
    dest_proc: &Proc,
    dest_frame: &Value,
    dest: usize,
) -> Result<(), CompileError> {
    if std::ptr::eq(src_proc, dest_proc) && src_frame == dest_frame && src == dest {
        return Ok(());
    }
    copy_slot(
        builder, src_proc, src_frame, src, dest_proc, dest_frame, dest,
    )?;
    release_managed(builder, src_proc, src_frame, src)
}

/// Return the old share and install the new share before either can be dropped.
/// The builder supplies a scratch destination that cannot alias `place`.
pub(super) fn emit_replace(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    place: usize,
    value: usize,
) -> Result<(), CompileError> {
    let pointer = load_fat(builder, frame, proc.slots[place].offset)?;
    let ty = proc.slots[dest].ty;
    release_managed(builder, proc, frame, dest)?;
    for cell in 0..ty.cells() {
        let old = load_cell(builder, &pointer, cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, old)?;
        let new = load_cell(builder, frame, proc.slots[value].offset + cell, IrType::I16)?;
        store_cell(builder, &pointer, cell, new)?;
    }
    // `dest` takes the old place's existing share. The place takes a retained
    // copy of the input; clearing the input releases its prior share.
    if keeps(proc, ty) {
        retain_tree(builder, proc, &pointer, 0, ty)?;
    }
    release_managed(builder, proc, frame, value)
}

pub(super) fn emit_into_inner(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
) -> Result<(), CompileError> {
    let obj = load_fat(builder, frame, proc.slots[src].offset)?;
    let ty = proc.slots[dest].ty;
    release_managed(builder, proc, frame, dest)?;
    for cell in 0..ty.cells() {
        let word = load_cell(builder, &obj, OWNER_DATA + cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    retain_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
    // Zero frame size is a disarmed hook; real RFL frames always have headers.
    store_cell(builder, &obj, OWNER_CLEANUP + 1, Value::Constant(0))?;
    release_tree(builder, proc, &obj, OWNER_DATA, ty)?;
    release_managed(builder, proc, frame, src)
}

pub(super) fn emit_owned(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    value: usize,
    cleanup: usize,
    inner: Ty,
) -> Result<(), CompileError> {
    let size = i32_const(builder, OWNER_DATA + inner.cells())?;
    let raw = builder
        .build_call(Value::Global("malloc".into()), vec![size], ptr_ty())
        .ir()?
        .unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder, FAIL_ALLOC)?;
    builder.create_block(ok).ir()?;
    store_cell(builder, &obj, 0, Value::Constant(1))?;
    copy_to_offset(builder, proc, frame, cleanup, &obj, OWNER_CLEANUP)?;
    copy_to_offset(builder, proc, frame, value, &obj, OWNER_DATA)?;
    if proc.resources.contains(inner) {
        release_managed(builder, proc, frame, value)?;
    }
    release_managed(builder, proc, frame, dest)?;
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    store_cell(builder, frame, proc.slots[dest].offset, addr)?;
    store_cell(builder, frame, proc.slots[dest].offset + 1, bank)
}

pub(super) fn emit_owner_drops(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    if built.collections.owners.is_empty() {
        return Ok(());
    }
    // Share the CPS driver across payload types instead of cloning every
    // reachable function's dispatch arm into each destructor.
    builder.create_function("rfl_run_cleanup".into(), IrType::Void);
    builder.add_parameter(0, ptr_ty());
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    run_cleanup(builder, built, &fat_temp(0, BankTag::Mixed))?;
    builder.build_return(None).ir()?;
    module.add_function(builder.finish_function().unwrap());
    for (id, inner) in built.collections.owners.iter().enumerate() {
        builder.create_function(format!("rfl_drop_owner_{id}"), IrType::Void);
        builder.add_parameter(0, IrType::I16);
        builder.add_parameter(1, IrType::I16);
        let entry = builder.new_label();
        builder.create_block(entry).ir()?;
        let obj = fat_from_parts(builder, Value::Temp(0), Value::Temp(1))?;
        let refs = load_cell(builder, &obj, 0, IrType::I16)?;
        let last = builder
            .build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1)
            .ir()?;
        let destroy = builder.new_label();
        let dec = builder.new_label();
        let done = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(last), destroy, dec)
            .ir()?;
        builder.create_block(dec).ir()?;
        let next = builder
            .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
            .ir()?;
        store_cell(builder, &obj, 0, Value::Temp(next))?;
        builder.build_branch(done).ir()?;
        builder.create_block(destroy).ir()?;
        builder
            .build_call(
                Value::Global("rfl_run_cleanup".into()),
                vec![obj.clone()],
                IrType::Void,
            )
            .ir()?;
        release_object_field(builder, built, &obj, OWNER_DATA, *inner)?;
        release_object_field(builder, built, &obj, OWNER_CLEANUP, Ty::Func(0))?;
        builder
            .build_call(Value::Global("free".into()), vec![obj], IrType::Void)
            .ir()?;
        builder.build_branch(done).ir()?;
        builder.create_block(done).ir()?;
        builder.build_return(None).ir()?;
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}

/// A finalizer is an ordinary synchronous RFL `fn(&mut T)->unit`. Run its
/// resumable frames on the same arena and rewind them before releasing T.
/// The unit root needs no caller/result storage and returns a null continuation.
fn run_cleanup(builder: &mut IrBuilder, built: &Built, obj: &Value) -> Result<(), CompileError> {
    let id = load_cell(builder, obj, OWNER_CLEANUP, IrType::I16)?;
    let words = load_cell(builder, obj, OWNER_CLEANUP + 1, IrType::I16)?;
    let disarmed = builder.new_label();
    let active = builder.new_label();
    let zero = builder
        .build_binary(
            IrBinaryOp::Eq,
            words.clone(),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(zero), disarmed, active)
        .ir()?;
    builder.create_block(disarmed).ir()?;
    builder.build_return(None).ir()?;
    builder.create_block(active).ir()?;
    let words = as_i32(builder, words)?;
    let raw = builder
        .build_call(
            Value::Global("rfl_frame_alloc".into()),
            vec![words],
            ptr_ty(),
        )
        .ir()?
        .unwrap();
    let frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &frame, FUNC_ID, id)?;
    let caller = gep(builder, &frame, CALLER)?;
    builder.build_store(null_ptr(), caller, ptr_ty()).ir()?;
    store_cell(builder, &frame, DEST, Value::Constant(0))?;
    let payload = gep(builder, obj, OWNER_DATA)?;
    let arg = gep(builder, &frame, PAYLOAD)?;
    builder.build_store(payload, arg, ptr_ty()).ir()?;
    let environment = load_fat(builder, obj, OWNER_CLEANUP + 2)?;
    let env_slot = gep(builder, &frame, PAYLOAD + 2)?;
    builder.build_store(environment, env_slot, ptr_ty()).ir()?;
    let current = builder.build_alloca(ptr_ty(), None).ir()?;
    let next_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_store(frame, current.clone(), ptr_ty()).ir()?;
    let head = builder.new_label();
    let merge = builder.new_label();
    let done = builder.new_label();
    let again = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let cur = load_ptr_slot(builder, &current)?;
    let id = load_cell(builder, &cur, FUNC_ID, IrType::I16)?;
    emit_dispatch(builder, built, &cur, &next_slot, id, merge, None)?;
    builder.create_block(merge).ir()?;
    let next = load_ptr_slot(builder, &next_slot)?;
    let finished = ptr_is_null(builder, &next)?;
    builder.build_branch_cond(finished, done, again).ir()?;
    builder.create_block(again).ir()?;
    builder.build_store(next, current, ptr_ty()).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}
