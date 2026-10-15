//! Element ownership is attached to the buffer, shared by vectors and slices.
use super::*;

fn buffer_drop_name(elem: Ty) -> String {
    let key: String = format!("{elem:?}")
        .chars()
        .filter(char::is_ascii_alphanumeric)
        .collect();
    format!("rfl_drop_buffer_{key}")
}

pub(super) fn release_buffer(
    builder: &mut IrBuilder,
    types: &crate::typed::CollectionTypes,
    addr: Value,
    bank: Value,
    ty: Ty,
) -> Result<(), CompileError> {
    if ty == Ty::String {
        return adjust_refcount(builder, addr, bank, false);
    }
    release_element_buffer(builder, addr, bank, types.element(ty))
}

pub(super) fn release_element_buffer(
    builder: &mut IrBuilder,
    addr: Value,
    bank: Value,
    elem: Ty,
) -> Result<(), CompileError> {
    // call_if_live uses argument 1 as its null check.
    call_if_live(builder, &buffer_drop_name(elem), vec![bank, addr])
}

pub(super) fn emit_buffer_drops(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    let mut elements = Vec::new();
    for elem in built
        .collections
        .vectors
        .iter()
        .chain(&built.collections.slices)
    {
        if !elements.contains(elem) {
            elements.push(*elem);
        }
    }
    for elem in elements {
        builder.create_function(buffer_drop_name(elem), IrType::Void);
        builder.add_parameter(0, IrType::I16); // bank
        builder.add_parameter(1, IrType::I16); // address
        let entry = builder.new_label();
        builder.create_block(entry).ir()?;
        let obj = fat_from_parts(builder, Value::Temp(1), Value::Temp(0))?;
        let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
        let last = builder
            .build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1)
            .ir()?;
        let drop = builder.new_label();
        let dec = builder.new_label();
        let done = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(last), drop, dec)
            .ir()?;
        builder.create_block(dec).ir()?;
        let next = builder
            .build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16)
            .ir()?;
        store_object_cell(builder, &obj, VEC_REFS, Value::Temp(next))?;
        builder.build_branch(done).ir()?;
        builder.create_block(drop).ir()?;
        // Scalar buffers need no per-element walk.
        if built.procs.iter().any(|p| keeps(p, elem)) {
            let len = load_count(builder, &obj, VEC_LEN)?;
            element_loop(builder, &obj, len, elem.cells(), |b, place| {
                release_object_field(b, built, place, 0, elem)
            })?;
        }
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

pub(super) fn retain_buffer_elements(
    builder: &mut IrBuilder,
    proc: &Proc,
    obj: &Value,
    len: Value,
    elem: Ty,
) -> Result<(), CompileError> {
    if !keeps(proc, elem) {
        return Ok(());
    }
    element_loop(builder, obj, len, elem.cells(), |b, place| {
        retain_managed_through(b, proc, place, elem)
    })
}

fn element_loop(
    builder: &mut IrBuilder,
    obj: &Value,
    len: Value,
    width: i64,
    mut visit: impl FnMut(&mut IrBuilder, &Value) -> Result<(), CompileError>,
) -> Result<(), CompileError> {
    let cursor = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), cursor.clone(), COUNT)
        .ir()?;
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(cursor.clone(), COUNT).ir()?);
    let more = builder
        .build_binary(IrBinaryOp::Ult, i.clone(), len, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let scaled = scale_index(builder, i.clone(), width)?;
    let offset = builder
        .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
        .ir()?;
    let place = builder
        .build_pointer_offset(obj.clone(), Value::Temp(offset), ptr_ty())
        .ir()?;
    visit(builder, &place)?;
    let next = builder
        .build_binary(IrBinaryOp::Add, i, Value::Constant(1), COUNT)
        .ir()?;
    builder.build_store(Value::Temp(next), cursor, COUNT).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}
