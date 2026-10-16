//! Address projections preserve storage identity; mutable vector ancestors
//! detach before their elements can be exposed through a reference.
use super::*;

pub(super) fn emit_borrow(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    borrow: &Place,
    mutable: bool,
) -> Result<(), CompileError> {
    let info = &proc.slots[borrow.root];
    let mut place = if borrow.indirect {
        load_fat(builder, frame, info.offset)?
    } else if info.shared {
        let obj = load_fat(builder, frame, info.offset)?;
        gep(builder, &obj, 1)?
    } else {
        gep(builder, frame, info.offset)?
    };
    for projection in &borrow.projections {
        place = match projection {
            PlaceStep::Owned => {
                let object=load_fat(builder,&place,0)?;
                gep(builder,&object,OWNER_DATA)?
            }
            PlaceStep::Field { offset } => gep(builder, &place, *offset)?,
            PlaceStep::Index {
                index,
                container,
                elem,
            } => {
                let at = load_slot(builder, proc, frame, *index)?;
                match container {
                    Ty::Array { len, .. } => {
                        let len = i32_const(builder, *len as i64)?;
                        check_index(builder, at.clone(), len)?;
                        let scaled = scale_index(builder, at, elem.cells())?;
                        builder.build_pointer_offset(place, scaled, ptr_ty()).ir()?
                    }
                    Ty::Vec(_) | Ty::Slice(_) => {
                        let mut obj = load_fat(builder, &place, 0)?;
                        let bad = builder.new_label();
                        let have = builder.new_label();
                        let missing = ptr_is_null(builder, &obj)?;
                        builder.build_branch_cond(missing, bad, have).ir()?;
                        builder.create_block(bad).ir()?;
                        fail(builder, FAIL_BOUNDS)?;
                        builder.build_return(Some(null_ptr())).ir()?;
                        builder.create_block(have).ir()?;
                        let len = if matches!(container, Ty::Slice(_)) {
                            load_count_at(builder, &place, SLICE_LEN)?
                        } else {
                            load_count(builder, &obj, VEC_LEN)?
                        };
                        check_index(builder, at.clone(), len)?;
                        if mutable {
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
                            detach_vec(builder, proc, &place, &obj, *elem)?;
                            builder.build_branch(write).ir()?;
                            builder.create_block(write).ir()?;
                            obj = load_fat(builder, &place, 0)?;
                        }
                        let at = if matches!(container, Ty::Slice(_)) {
                            let start = load_count_at(builder, &place, SLICE_START)?;
                            Value::Temp(
                                builder
                                    .build_binary(IrBinaryOp::Add, at, start, COUNT)
                                    .ir()?,
                            )
                        } else {
                            at
                        };
                        let scaled = scale_index(builder, at, elem.cells())?;
                        let offset = builder
                            .build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), COUNT)
                            .ir()?;
                        builder
                            .build_pointer_offset(obj, Value::Temp(offset), ptr_ty())
                            .ir()?
                    }
                    _ => return Err(CompileError::plain("compiler bug: invalid indexed place")),
                }
            }
        };
    }
    let addr = low16_of_ptr(builder, &place)?;
    let bank = bank16_of_ptr(builder, &place)?;
    store_cell(builder, frame, proc.slots[dest].offset, addr)?;
    store_cell(builder, frame, proc.slots[dest].offset + 1, bank)
}

fn check_index(builder: &mut IrBuilder, index: Value, len: Value) -> Result<(), CompileError> {
    let valid = builder
        .build_binary(IrBinaryOp::Ult, index, len, IrType::I1)
        .ir()?;
    let ok = builder.new_label();
    let bad = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(valid), ok, bad)
        .ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_BOUNDS)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    Ok(())
}
