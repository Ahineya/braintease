//! Union storage has no single active IR field type. Normalize its initialized
//! member into cell constants, retaining relocatable pointers as two-cell atoms.
use crate::ir::{IrType, Value};
use crate::CompilerError;

fn error(message: &str) -> CompilerError {
    CompilerError::semantic_error(message.into(), rcc_common::SourceLocation::new_simple(0, 0))
}

pub(super) fn cells(value: Value, ty: &IrType) -> Result<Vec<Value>, CompilerError> {
    let size = ty.size_in_words().ok_or_else(|| error("Unsized union initializer member"))?;
    let mut output = Vec::new();
    let mut used = 0;
    match (value, ty) {
        (Value::Constant(n), _) => {
            if size > 4 { return Err(error("Aggregate union member requires an aggregate initializer")); }
            for i in 0..size { output.push(Value::Constant(((n as u64 >> (16 * i)) & 65535) as i64)); }
            used = size;
        }
        (v @ (Value::FatPtr(_) | Value::Function(_) | Value::Global(_)), _) if size == 2 => {
            output.push(v); used = 2;
        }
        (Value::ConstantArray(values), IrType::Array { element_type, .. }) => {
            for n in values {
                let part = cells(Value::Constant(n), element_type)?;
                used += span(&part); output.extend(part);
            }
        }
        (Value::ConstantAggregate(values), IrType::Array { element_type, .. }) => {
            for v in values {
                // A nested union has already been normalized to cell storage.
                // Its pointer atoms occupy two cells rather than one element.
                let part = if **element_type == IrType::I16 &&
                    matches!(v, Value::FatPtr(_) | Value::Function(_) | Value::Global(_)) {
                    vec![v]
                } else { cells(v, element_type)? };
                used += span(&part); output.extend(part);
            }
        }
        (Value::ConstantAggregate(values), IrType::Struct { fields, .. }) => {
            if values.len() > fields.len() { return Err(error("Too many union member struct initializers")); }
            for (v, field) in values.into_iter().zip(fields) {
                let part = cells(v, field)?;
                used += span(&part); output.extend(part);
            }
        }
        (Value::ConstantAggregate(mut values), _) if values.len() <= 1 => {
            if let Some(v) = values.pop() { return cells(v, ty); }
        }
        _ => return Err(error("Unsupported constant union member initializer")),
    }
    if used > size { return Err(error("Union initializer exceeds its member storage")); }
    output.extend((used..size).map(|_| Value::Constant(0)));
    Ok(output)
}

fn span(values: &[Value]) -> u64 {
    values.iter().map(|v| if matches!(v, Value::FatPtr(_) | Value::Function(_) | Value::Global(_)) { 2 } else { 1 }).sum()
}
