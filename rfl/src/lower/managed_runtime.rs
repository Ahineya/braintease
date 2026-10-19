//! Share leaf release operations across expressions and scope exits. Each helper
//! clears the caller's storage using the same typed destruction rules as before.
use super::*;

pub(super) fn release_helper_name(ty: Ty) -> Option<String> {
    Some(format!(
        "rfl_release_{}",
        match ty {
            Ty::Func(_) => "function".into(),
            Ty::String => "string".into(),
            Ty::ErasedOwner | Ty::SharedOwner => "erased".into(),
            Ty::Owned(id) => format!("owner_{id}"),
            Ty::Adt(id) => format!("adt_{id}"),
            Ty::Vec(id) => format!("vector_{id}"),
            Ty::Slice(id) => format!("slice_{id}"),
            _ => return None,
        }
    ))
}

pub(super) fn emit_managed_releases(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    if !built.need_heap {
        return Ok(());
    }
    let mut types = vec![Ty::Func(0), Ty::String, Ty::ErasedOwner];
    types.extend((0..built.collections.owners.len()).map(|id| Ty::Owned(id as u32)));
    types.extend((0..built.adts.len()).map(|id| Ty::Adt(id as u32)));
    types.extend((0..built.collections.vectors.len()).map(|id| Ty::Vec(id as u32)));
    types.extend((0..built.collections.slices.len()).map(|id| Ty::Slice(id as u32)));
    let proc = built.procs.first().expect("main exists");
    for ty in types {
        builder.create_function(release_helper_name(ty).unwrap(), IrType::Void);
        builder.add_parameter(0, ptr_ty());
        let entry = builder.new_label();
        builder.create_block(entry).ir()?;
        release_managed_body(builder, proc, &fat_temp(0, BankTag::Mixed), ty)?;
        builder.build_return(None).ir()?;
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}
