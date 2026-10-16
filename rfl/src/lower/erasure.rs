//! Opaque owners retain the original allocation and its typed destructor.
use super::*;

pub(super) fn emit_erase_owner(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    owner: u32,
) -> Result<(), CompileError> {
    if owner > u16::MAX as u32 {
        return Err(CompileError::plain("too many erased owner types"));
    }
    let size = i32_const(builder, 4)?;
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
    store_cell(builder, &obj, 1, Value::Constant(owner as i64))?;
    copy_to_offset(builder, proc, frame, src, &obj, 2)?;
    release_managed(builder, proc, frame, src)?;
    release_managed(builder, proc, frame, dest)?;
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    store_cell(builder, frame, proc.slots[dest].offset, addr)?;
    store_cell(builder, frame, proc.slots[dest].offset + 1, bank)
}

pub(super) fn emit_erased_drop(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    // Empty containers can mention ErasedOwner without constructing one.
    // Their generated drop glue still needs this definition.
    if !built.need_heap {
        return Ok(());
    }
    builder.create_function("rfl_drop_erased_owner".into(), IrType::Void);
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
    let tag = load_cell(builder, &obj, 1, IrType::I16)?;
    let addr = load_cell(builder, &obj, 2, IrType::I16)?;
    let bank = load_cell(builder, &obj, 3, IrType::I16)?;
    let free = builder.new_label();
    let owners: std::collections::BTreeSet<u32> = built
        .procs
        .iter()
        .flat_map(|p| &p.segs)
        .flat_map(|s| &s.insns)
        .filter_map(|i| match i {
            Insn::EraseOwner { owner, .. } => Some(*owner),
            _ => None,
        })
        .collect();
    for id in owners {
        let yes = builder.new_label();
        let no = builder.new_label();
        let eq = builder
            .build_binary(
                IrBinaryOp::Eq,
                tag.clone(),
                Value::Constant(id as i64),
                IrType::I1,
            )
            .ir()?;
        builder.build_branch_cond(Value::Temp(eq), yes, no).ir()?;
        builder.create_block(yes).ir()?;
        builder
            .build_call(
                Value::Global(format!("rfl_drop_owner_{id}")),
                vec![addr.clone(), bank.clone()],
                IrType::Void,
            )
            .ir()?;
        builder.build_branch(free).ir()?;
        builder.create_block(no).ir()?;
    }
    // Tags are compiler-written. Fail explicitly if storage was corrupted.
    fail(builder, FAIL_UNREACHABLE)?;
    builder.build_return(None).ir()?;
    builder.create_block(free).ir()?;
    builder
        .build_call(Value::Global("free".into()), vec![obj], IrType::Void)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    builder.build_return(None).ir()?;
    module.add_function(builder.finish_function().unwrap());
    Ok(())
}
