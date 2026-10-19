//! Transfer one existing share. Only owning pointer cells are disarmed; tags,
//! scalar fields, lengths and borrowed/raw pointers keep their old values.
use super::*;

pub(super) fn clear_transferred(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    offset: i64,
    ty: Ty,
) -> Result<(), CompileError> {
    let pointer = match ty {
        Ty::String
        | Ty::Vec(_)
        | Ty::Slice(_)
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::SharedOwner => Some(offset),
        Ty::Adt(_) => Some(offset + 1),
        Ty::Func(_) => Some(offset + 2),
        Ty::Struct { id, .. } => {
            return clear_fields(builder, proc, frame, offset, &proc.structs[id as usize])
        }
        Ty::Tuple { id, .. } => {
            return clear_fields(builder, proc, frame, offset, &proc.tuples[id as usize])
        }
        Ty::Array { id, len, width } => {
            let ty = proc.collections.arrays[id as usize];
            if keeps(proc, ty) {
                for i in 0..len {
                    clear_transferred(
                        builder,
                        proc,
                        frame,
                        offset + i64::from(i) * i64::from(width),
                        ty,
                    )?;
                }
            }
            None
        }
        _ => None,
    };
    if let Some(at) = pointer {
        store_cell(builder, frame, at, Value::Constant(0))?;
        store_cell(builder, frame, at + 1, Value::Constant(0))?;
    }
    Ok(())
}
fn clear_fields(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    mut offset: i64,
    fields: &[Ty],
) -> Result<(), CompileError> {
    for &ty in fields {
        clear_transferred(builder, proc, frame, offset, ty)?;
        offset += ty.cells();
    }
    Ok(())
}

/// Destination storage is disjoint and already released (or uninitialized).
pub(super) fn transfer_to_offset(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    src: usize,
    dest: &Value,
    offset: i64,
) -> Result<(), CompileError> {
    let info = &proc.slots[src];
    if info.ty == Ty::Unit {
        return Ok(());
    }
    if info.shared {
        return Err(CompileError::plain(
            "cannot transfer a shared capture's storage",
        ));
    }
    if cell_wise(info.ty) {
        copy_frame_cells(builder, frame, info.offset, dest, offset, info.ty.cells())?;
    } else {
        let value = load_slot(builder, proc, frame, src)?;
        let at = gep(builder, dest, offset)?;
        builder.build_store(value, at, ir_ty(info.ty)).ir()?;
    }
    clear_transferred(builder, proc, frame, info.offset, info.ty)
}
