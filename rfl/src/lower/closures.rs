//! Closures support for RFL lowering.

use super::*;

pub(super) fn emit_capture_loads(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    let env_at = proc
        .env_at
        .expect("a capturing function has an environment slot");
    let addr = load_cell(builder, frame, env_at, IrType::I16)?;
    let bank = load_cell(builder, frame, env_at + 1, IrType::I16)?;
    let env = fat_from_parts(builder, addr, bank)?;
    for cap in &proc.captures {
        let dest = proc.slots[cap.slot].offset;
        for index in 0..cap.cells {
            let value = load_cell(builder, &env, cap.offset + index, IrType::I16)?;
            store_cell(builder, frame, dest + index, value)?;
        }
        if cap.shared {
            let addr = load_cell(builder, frame, dest, IrType::I16)?;
            let bank = load_cell(builder, frame, dest + 1, IrType::I16)?;
            adjust_refcount(builder, addr, bank, true)?;
        } else if keeps(proc, proc.slots[cap.slot].ty) {
            retain_managed(builder, proc, frame, cap.slot)?;
        }
    }
    Ok(())
}

pub(super) fn emit_env(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    captures: &[LowerCap],
) -> Result<(Value, Value), CompileError> {
    let words = 1 + captures.iter().map(|cap| cap.cells).sum::<i64>();
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
    for cap in captures {
        let parent = proc.binding_slots[cap.parent as usize];
        let src = proc.slots[parent].offset;
        for index in 0..cap.cells {
            let value = load_cell(builder, frame, src + index, IrType::I16)?;
            store_object_cell(builder, &obj, cap.offset + index, value)?;
        }
        if cap.shared {
            let addr = load_cell(builder, frame, src, IrType::I16)?;
            let bank = load_cell(builder, frame, src + 1, IrType::I16)?;
            adjust_refcount(builder, addr, bank, true)?;
        } else if keeps(proc, proc.slots[parent].ty) {
            retain_managed(builder, proc, frame, parent)?;
        }
    }
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    Ok((addr, bank))
}
