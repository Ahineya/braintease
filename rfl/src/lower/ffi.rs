//! Ffi support for RFL lowering.

use super::*;

pub(super) fn emit_cstr(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    dest: usize,
    bytes: &[u16],
) -> Result<(), CompileError> {
    let index = built
        .cstrs
        .iter()
        .position(|found| found == bytes)
        .expect("c string");
    let fat = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Global(format!("__rfl_cstr_{index}"))),
        bank: BankTag::Global,
    });
    let ptr = gep(builder, frame, proc.slots[dest].offset)?;
    builder.build_store(fat, ptr, ptr_ty()).ir()
}

pub(super) fn emit_extern(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    dest: usize,
    name: &str,
    args: &[usize],
    ret: Ty,
    fixed: usize,
) -> Result<(), CompileError> {
    if super::browser::intrinsic(name) {
        return super::browser::exchange(builder, proc, frame, dest, args.first().copied());
    }
    let mut values = Vec::new();
    for slot in args {
        let ty = proc.slots[*slot].ty;
        if matches!(ty, Ty::Ptr(_) | Ty::CFn(_)) {
            values.push(load_fat(builder, frame, proc.slots[*slot].offset)?);
        } else if repr_struct(&built.repr_c, ty) {
            values.push(gep(builder, frame, proc.slots[*slot].offset)?);
        } else {
            values.push(load_slot(builder, proc, frame, *slot)?);
        }
    }
    let stack_from = if fixed < args.len() {
        Some(fixed)
    } else {
        None
    };
    let result = builder
        .build_call_ex(
            Value::Global(name.to_string()),
            values,
            c_ret_ir(ret, &built.repr_c),
            stack_from,
        )
        .ir()?;
    if ret == Ty::Unit {
        return Ok(());
    }
    let Some(result) = result else {
        return Err(CompileError::plain(format!(
            "`{name}` did not return a value"
        )));
    };
    if repr_struct(&built.repr_c, ret) {
        let src = fat_temp(result, BankTag::Stack);
        copy_words(builder, &src, frame, proc.slots[dest].offset, ret.cells())
    } else if matches!(ret, Ty::Ptr(_) | Ty::CFn(_)) {
        let ptr = gep(builder, frame, proc.slots[dest].offset)?;
        builder.build_store(Value::Temp(result), ptr, ptr_ty()).ir()
    } else {
        store_slot(builder, proc, frame, dest, Value::Temp(result))
    }
}

pub(super) fn c_param_ir(ty: Ty, repr: &[bool]) -> IrType {
    if matches!(ty, Ty::Ptr(_) | Ty::CFn(_)) || repr_struct(repr, ty) {
        ptr_ty()
    } else {
        ir_ty(ty)
    }
}

pub(super) fn c_ret_ir(ty: Ty, repr: &[bool]) -> IrType {
    if ty == Ty::Unit {
        IrType::Void
    } else if matches!(ty, Ty::Ptr(_) | Ty::CFn(_)) || repr_struct(repr, ty) {
        ptr_ty()
    } else {
        ir_ty(ty)
    }
}

pub(super) fn emit_c_apply(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    callee: usize,
    args: &[usize],
    ret: Ty,
    repr: &[bool],
) -> Result<(), CompileError> {
    let mut values = Vec::new();
    for slot in args {
        let ty = proc.slots[*slot].ty;
        if matches!(ty, Ty::Ptr(_) | Ty::CFn(_)) {
            values.push(load_fat(builder, frame, proc.slots[*slot].offset)?);
        } else if repr_struct(repr, ty) {
            values.push(gep(builder, frame, proc.slots[*slot].offset)?);
        } else {
            values.push(load_slot(builder, proc, frame, *slot)?);
        }
    }
    let target = load_fat(builder, frame, proc.slots[callee].offset)?;
    let result = builder
        .build_call_ex(target, values, c_ret_ir(ret, repr), None)
        .ir()?;
    if ret == Ty::Unit {
        return Ok(());
    }
    let Some(result) = result else {
        return Err(CompileError::plain(
            "a C function pointer did not return a value",
        ));
    };
    if repr_struct(repr, ret) {
        let src = fat_temp(result, BankTag::Stack);
        copy_words(builder, &src, frame, proc.slots[dest].offset, ret.cells())
    } else if matches!(ret, Ty::Ptr(_) | Ty::CFn(_)) {
        let ptr = gep(builder, frame, proc.slots[dest].offset)?;
        builder.build_store(Value::Temp(result), ptr, ptr_ty()).ir()
    } else {
        store_slot(builder, proc, frame, dest, Value::Temp(result))
    }
}

pub(super) fn extern_fn(name: &str, ret: IrType, params: Vec<IrType>) -> Function {
    let mut func = Function::new(name.to_string(), ret);
    func.is_external = true;
    for (index, ty) in params.into_iter().enumerate() {
        func.add_parameter(index as u32, ty);
    }
    func
}
