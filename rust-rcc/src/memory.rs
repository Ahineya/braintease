//! Octet-cell legalization. No changes to the existing C16 object ABI.
use crate::layout::{self, Result};
use rcc_ir::{
    BankTag,
    ir::{FatPointer, IrBinaryOp as B, IrBuilder, IrType as T, IrUnaryOp as U, Value},
};
use rustc_middle::ty::{Ty, TyCtxt};

pub fn pointer(value: Value) -> Value {
    match value {
        Value::FatPtr(_) => value,
        _ => Value::FatPtr(FatPointer {
            addr: Box::new(value),
            bank: BankTag::Mixed,
        }),
    }
}

pub fn offset(b: &mut IrBuilder, p: Value, cells: Value) -> Result<Value> {
    if cells == Value::Constant(0) {
        return Ok(p);
    }
    b.build_pointer_offset(pointer(p), cells, layout::pointer_type())
}

pub fn unary(b: &mut IrBuilder, op: U, v: Value, ty: T) -> Result<Value> {
    Ok(Value::Temp(b.build_unary(op, v, ty)?))
}

pub fn binary(b: &mut IrBuilder, op: B, x: Value, y: Value, ty: T) -> Result<Value> {
    Ok(Value::Temp(b.build_binary(op, x, y, ty)?))
}

pub fn alloca(b: &mut IrBuilder, bytes: u64, align: u64) -> Result<Value> {
    // Keep this proof of concept bounded: no accidental megabytes of unrolled IR.
    if bytes > 1024 {
        return Err(format!("prototype object exceeds 1024 Rust bytes: {bytes}"));
    }
    let p = b.build_alloca(
        T::Array {
            size: bytes.max(1) + align - 1,
            element_type: Box::new(T::I16),
        },
        None,
    )?;
    if align <= 1 {
        return Ok(p);
    }
    let bits = unary(b, U::PtrToInt, p, T::I32)?;
    let bits = binary(b, B::Add, bits, Value::Constant(align as i64 - 1), T::I32)?;
    let bits = binary(b, B::And, bits, Value::Constant(-(align as i64)), T::I32)?;
    Ok(pointer(unary(
        b,
        U::IntToPtr,
        bits,
        layout::pointer_type(),
    )?))
}

pub fn load_bits(b: &mut IrBuilder, p: Value, bytes: u64, ty: T) -> Result<Value> {
    let mut result = Value::Constant(0);
    for i in 0..bytes {
        let address = offset(b, p.clone(), Value::Constant(i as i64))?;
        let byte = Value::Temp(b.build_load(address, T::I16)?);
        let byte = binary(b, B::And, byte, Value::Constant(255), T::I16)?;
        let byte = if ty == T::I16 {
            byte
        } else {
            unary(b, U::ZExt, byte, ty.clone())?
        };
        let byte = if i == 0 {
            byte
        } else {
            binary(b, B::Shl, byte, Value::Constant((8 * i) as i64), ty.clone())?
        };
        result = binary(b, B::Or, result, byte, ty.clone())?;
    }
    Ok(result)
}

pub fn store_bits(b: &mut IrBuilder, p: Value, value: Value, bytes: u64, ty: T) -> Result<()> {
    for i in 0..bytes {
        let part = if i == 0 {
            value.clone()
        } else {
            binary(
                b,
                B::LShr,
                value.clone(),
                Value::Constant((8 * i) as i64),
                ty.clone(),
            )?
        };
        let part = binary(b, B::And, part, Value::Constant(255), ty.clone())?;
        let part = if ty == T::I16 {
            part
        } else {
            unary(b, U::Trunc, part, T::I16)?
        };
        let address = offset(b, p.clone(), Value::Constant(i as i64))?;
        b.build_store(part, address, T::I16)?;
    }
    Ok(())
}

pub fn load<'tcx>(b: &mut IrBuilder, tcx: TyCtxt<'tcx>, p: Value, ty: Ty<'tcx>) -> Result<Value> {
    let carrier = layout::carrier(tcx, ty)?.ok_or_else(|| format!("non-scalar load {ty}"))?;
    let bytes = layout::size(tcx, ty)?;
    let pointer_value = matches!(carrier, T::FatPtr(_));
    let register = if pointer_value { T::I32 } else { carrier };
    let mut value = load_bits(b, p, bytes, register)?;
    if bytes == 1 && layout::signed(ty) {
        value = binary(b, B::Shl, value, Value::Constant(8), T::I16)?;
        value = binary(b, B::AShr, value, Value::Constant(8), T::I16)?;
    }
    if pointer_value {
        value = pointer(unary(b, U::IntToPtr, value, layout::pointer_type())?);
    }
    Ok(value)
}

pub fn store<'tcx>(
    b: &mut IrBuilder,
    tcx: TyCtxt<'tcx>,
    p: Value,
    mut value: Value,
    ty: Ty<'tcx>,
) -> Result<()> {
    let carrier = layout::carrier(tcx, ty)?.ok_or_else(|| format!("non-scalar store {ty}"))?;
    let register = if matches!(carrier, T::FatPtr(_)) {
        value = unary(b, U::PtrToInt, pointer(value), T::I32)?;
        T::I32
    } else {
        carrier
    };
    store_bits(b, p, value, layout::size(tcx, ty)?, register)
}

pub fn copy(b: &mut IrBuilder, destination: Value, source: Value, bytes: u64) -> Result<()> {
    if bytes > 1024 {
        return Err("prototype copy exceeds 1024 Rust bytes".into());
    }
    // Read first, write second: also correct when source and destination overlap.
    let mut values = Vec::new();
    for i in 0..bytes {
        let address = offset(b, source.clone(), Value::Constant(i as i64))?;
        values.push(Value::Temp(b.build_load(address, T::I16)?));
    }
    for (i, value) in values.into_iter().enumerate() {
        let address = offset(b, destination.clone(), Value::Constant(i as i64))?;
        b.build_store(value, address, T::I16)?;
    }
    Ok(())
}
