use super::*;
use rcc_ir::ir::{IrType, Value};

pub(super) fn initializers(compiler: &Compiler) -> Result<Vec<(u64, u64, u64)>, String> {
    Ok(image_initializers(compiler)?
        .into_iter()
        .map(|(p, v, n, _)| (p, v, n))
        .collect())
}

pub(super) fn image_initializers(
    compiler: &Compiler,
) -> Result<Vec<(u64, u64, u64, bool)>, String> {
    image_data(compiler).map(|v| v.0)
}
pub(super) fn image_data(
    compiler: &Compiler,
) -> Result<
    (
        Vec<(u64, u64, u64, bool)>,
        Vec<rcc_ir::native_image::Relocation>,
    ),
    String,
> {
    let mut output = Vec::new();
    let mut relocations = Vec::new();
    for global in &compiler.ir.globals {
        if let Some(value) = &global.initializer {
            flatten(
                compiler,
                compiler.globals[&global.name],
                &global.var_type,
                value,
                &mut output,
                &mut relocations,
            )?;
        }
    }
    Ok((output, relocations))
}
fn offset(pointer: u64, cells: u64) -> Result<u64, String> {
    let layout = Target::Native.layout();
    let bank = pointer >> 16;
    let addr = pointer & 65535;
    let linear = bank * u64::from(layout.bank_cells) + addr;
    let linear = linear
        .checked_add(cells)
        .ok_or("native initializer offset overflow")?;
    Ok(((linear / u64::from(layout.bank_cells)) << 16) | (linear % u64::from(layout.bank_cells)))
}
fn constant(compiler: &Compiler, value: &Value) -> Result<u64, String> {
    match value {
        Value::Constant(n) => Ok(*n as u64),
        Value::Undef => Ok(0),
        Value::Global(name) | Value::Function(name) => compiler
            .globals
            .get(name)
            .or_else(|| compiler.codes.get(name))
            .or_else(|| {
                compiler.dynamic.as_ref().and_then(|m| {
                    m.imports
                        .iter()
                        .find(|i| i.symbol.name == *name)
                        .map(|i| &i.local)
                })
            })
            .copied()
            .ok_or_else(|| format!("unknown initializer symbol {name}")),
        Value::FatPtr(ptr) => {
            let value = constant(compiler, &ptr.addr)?;
            if matches!(ptr.addr.as_ref(), Value::Constant(_)) {
                let bank = match ptr.bank {
                    rcc_ir::BankTag::Global => compiler.global_bank,
                    rcc_ir::BankTag::Stack => 2,
                    rcc_ir::BankTag::Heap(b) => u64::from(b),
                    rcc_ir::BankTag::Null => 0,
                    _ => return Err("constant initializer pointer lacks bank".into()),
                };
                Ok((bank << 16) | (value & 65535))
            } else {
                Ok(value)
            }
        }
        Value::AddressOffset { base, cells } => {
            let value = constant(compiler, base)?;
            if *cells >= 0 {
                offset(value, *cells as u64)
            } else {
                let layout = Target::Native.layout();
                let linear = (value >> 16) * u64::from(layout.bank_cells) + (value & 65535);
                let linear = linear
                    .checked_sub(cells.unsigned_abs())
                    .ok_or("initializer points before memory")?;
                Ok(((linear / u64::from(layout.bank_cells)) << 16)
                    | (linear % u64::from(layout.bank_cells)))
            }
        }
        _ => Err(format!("nonconstant native initializer {value:?}")),
    }
}
fn flatten(
    compiler: &Compiler,
    pointer: u64,
    ty: &IrType,
    value: &Value,
    out: &mut Vec<(u64, u64, u64, bool)>,
    relocations: &mut Vec<rcc_ir::native_image::Relocation>,
) -> Result<(), String> {
    match (ty, value) {
        (IrType::Array { size, element_type }, Value::ConstantArray(values)) => {
            if values.len() as u64 > *size {
                return Err("too many native array initializer values".into());
            }
            let stride = Target::Native.layout().size_in_cells(element_type)?;
            for (i, value) in values.iter().enumerate() {
                flatten(
                    compiler,
                    offset(pointer, i as u64 * stride)?,
                    element_type,
                    &Value::Constant(*value),
                    out,
                    relocations,
                )?;
            }
        }
        (IrType::Array { size, element_type }, Value::ConstantAggregate(values)) => {
            if **element_type == IrType::I16
                && values.iter().all(|v| {
                    matches!(
                        v,
                        Value::Constant(_)
                            | Value::FatPtr(_)
                            | Value::Function(_)
                            | Value::Global(_)
                    )
                })
            {
                let mut cell = 0;
                for value in values {
                    let pointer_value = matches!(
                        value,
                        Value::FatPtr(_) | Value::Function(_) | Value::Global(_)
                    );
                    let width = if pointer_value { 2 } else { 1 };
                    if cell + width > *size {
                        return Err("cell initializer exceeds object storage".into());
                    }
                    let value_type = if pointer_value {
                        IrType::FatPtr(Box::new(IrType::I16))
                    } else {
                        IrType::I16
                    };
                    flatten(
                        compiler,
                        offset(pointer, cell)?,
                        &value_type,
                        value,
                        out,
                        relocations,
                    )?;
                    cell += width;
                }
                return Ok(());
            }
            if values.len() as u64 > *size {
                return Err("too many native aggregate values".into());
            }
            let stride = Target::Native.layout().size_in_cells(element_type)?;
            for (i, value) in values.iter().enumerate() {
                flatten(
                    compiler,
                    offset(pointer, i as u64 * stride)?,
                    element_type,
                    value,
                    out,
                    relocations,
                )?;
            }
        }
        (IrType::Struct { fields, .. }, Value::ConstantAggregate(values)) => {
            if values.len() > fields.len() {
                return Err("too many native struct initializer values".into());
            }
            let mut at = pointer;
            for (field, value) in fields.iter().zip(values) {
                flatten(compiler, at, field, value, out, relocations)?;
                at = offset(at, Target::Native.layout().size_in_cells(field)?)?;
            }
        }
        _ => {
            let words = Target::Native.layout().size_in_cells(ty)?;
            if words > 4 {
                return Err(format!("unsupported native initializer {value:?} for {ty}"));
            }
            if compiler.dynamic.is_some() {
                fn reference(
                    compiler: &Compiler,
                    value: &Value,
                ) -> Result<Option<(u64, bool, i64)>, String> {
                    match value {
                        Value::AddressOffset { base, cells } => {
                            Ok(reference(compiler, base)?.map(|(p, f, a)| (p, f, a + cells)))
                        }
                        Value::FatPtr(p) => {
                            if matches!(p.addr.as_ref(), Value::Constant(_))
                                && matches!(p.bank, rcc_ir::BankTag::Global)
                            {
                                Ok(Some((constant(compiler, value)?, false, 0)))
                            } else {
                                reference(compiler, &p.addr)
                            }
                        }
                        Value::Global(n) | Value::Function(n) => {
                            let p = constant(compiler, value)?;
                            let function = compiler.codes.contains_key(n)
                                || compiler
                                    .dynamic
                                    .as_ref()
                                    .unwrap()
                                    .imports
                                    .iter()
                                    .any(|i| i.symbol.name == *n && i.symbol.function);
                            Ok(Some((p, function, 0)))
                        }
                        _ => Ok(None),
                    }
                }
                if let Some((target, function, addend)) = reference(compiler, value)? {
                    if words != 2 || (function && addend != 0) {
                        return Err("invalid module pointer initializer".into());
                    }
                    let index =
                        ((pointer >> 16) - compiler.global_bank) * 64000 + (pointer & 65535);
                    relocations.push(rcc_ir::native_image::Relocation {
                        offset: index as u32,
                        target,
                        function,
                        addend,
                    });
                    return Ok(());
                }
            }
            fn token(compiler: &Compiler, value: &Value) -> bool {
                match value {
                    Value::Global(n) | Value::Function(n) => {
                        compiler.codes.contains_key(n) || compiler.dynamic.is_some()
                    }
                    Value::FatPtr(p) => {
                        token(compiler, &p.addr)
                            || (compiler.dynamic.is_some()
                                && matches!(p.bank, rcc_ir::BankTag::Global))
                    }
                    Value::AddressOffset { base, .. } => token(compiler, base),
                    _ => false,
                }
            }
            let constant = constant(compiler, value)?;
            // Both a fresh runtime and an executable template start zeroed.
            // Large sparse arrays must not emit one call/JSON tuple per zero.
            if constant != 0 {
                out.push((pointer, constant, words, token(compiler, value)));
            }
        }
    }
    Ok(())
}

pub(super) fn register_functions(compiler: &mut Compiler, value: &Value) -> Result<(), String> {
    match value {
        Value::Global(name) | Value::Function(name) if !compiler.globals.contains_key(name) => {
            compiler.function_token(name)?;
        }
        Value::FatPtr(pointer) => register_functions(compiler, &pointer.addr)?,
        Value::AddressOffset { base, .. } => register_functions(compiler, base)?,
        Value::ConstantAggregate(values) => {
            for value in values {
                register_functions(compiler, value)?;
            }
        }
        _ => {}
    }
    Ok(())
}
