//! Address constants must retain their symbol until link time. In particular,
//! evaluating &array[n] cannot emit a normal GEP outside a function.

use crate::ast::UnaryOp;
use crate::codegen::{expressions::TypedExpressionGenerator, CodegenError};
use crate::ir::{FatPointer, Value};
use crate::typed_ast::TypedExpr;
use crate::types::{BankTag, Type};
use crate::CompilerError;
use super::global_gen::scalar_initializer_constant;

pub(super) fn generate(
    gen: &mut TypedExpressionGenerator,
    expr: &TypedExpr,
) -> Result<Option<Value>, CompilerError> {
    match expr {
        TypedExpr::Variable { name, expr_type: Type::Function { .. }, .. } =>
            Ok(Some(Value::Function(name.clone()))),
        TypedExpr::Variable { expr_type: Type::Array { .. }, .. }
        | TypedExpr::ArrayIndex { expr_type: Type::Array { .. }, .. }
        | TypedExpr::MemberAccess { expr_type: Type::Array { .. }, .. } => lvalue(gen, expr),
        TypedExpr::StringLiteral { .. } => gen.generate(expr).map(Some),
        TypedExpr::Unary { op: UnaryOp::AddressOf, operand, .. } => lvalue(gen, operand),
        TypedExpr::Cast { operand, target_type: Type::Pointer { .. }, .. } => {
            if operand.get_type().is_integer() {
                Ok(scalar_initializer_constant(operand).map(|integer| {
                    let bits = integer as u32;
                    Value::FatPtr(FatPointer {
                        addr: Box::new(Value::Constant((bits & 0xffff) as i64)),
                        bank: if bits == 0 { BankTag::Null } else { BankTag::Heap((bits >> 16) as u16) },
                    })
                }))
            } else {
                generate(gen, operand)
            }
        }
        TypedExpr::PointerArithmetic { ptr, offset, elem_type, is_add, .. } => {
            let Some(index) = scalar_initializer_constant(offset) else { return Ok(None); };
            let Some(base) = generate(gen, ptr)? else { return Ok(None); };
            let size = elem_type.size_in_words().and_then(|n| i64::try_from(n).ok())
                .ok_or_else(address_overflow)?;
            let cells = index.checked_mul(size).and_then(|n| if *is_add { Some(n) } else { n.checked_neg() })
                .ok_or_else(address_overflow)?;
            add_cells(base, cells).map(Some)
        }
        TypedExpr::Conditional { condition, then_expr, else_expr, .. } => {
            let Some(condition) = scalar_initializer_constant(condition) else { return Ok(None); };
            generate(gen, if condition != 0 { then_expr } else { else_expr })
        }
        _ => Ok(None),
    }
}

fn lvalue(gen: &mut TypedExpressionGenerator, expr: &TypedExpr) -> Result<Option<Value>, CompilerError> {
    match expr {
        TypedExpr::Variable { name, expr_type: Type::Function { .. }, .. } =>
            Ok(Some(Value::Function(name.clone()))),
        TypedExpr::Variable { name, .. } => {
            if let Some(variable) = gen.variables.get(name) {
                // Static locals have a lexical name and a distinct internal
                // linker symbol. Automatic storage is not an address constant.
                return Ok(if matches!(variable.value, Value::Global(_)) {
                    Some(variable.as_fat_ptr())
                } else { None });
            }
            Ok(Some(Value::FatPtr(FatPointer {
                addr: Box::new(Value::Global(name.clone())), bank: BankTag::Global,
            })))
        }
        TypedExpr::StringLiteral { .. } => gen.generate(expr).map(Some),
        TypedExpr::ArrayIndex { array, index, elem_type, .. } => {
            let Some(index) = scalar_initializer_constant(index) else { return Ok(None); };
            let Some(base) = generate(gen, array)? else { return Ok(None); };
            let size = elem_type.size_in_words().and_then(|n| i64::try_from(n).ok())
                .ok_or_else(address_overflow)?;
            add_cells(base, index.checked_mul(size).ok_or_else(address_overflow)?).map(Some)
        }
        TypedExpr::MemberAccess { object, offset, is_pointer, .. } => {
            let base = if *is_pointer { generate(gen, object)? } else { lvalue(gen, object)? };
            let Some(base) = base else { return Ok(None); };
            add_cells(base, i64::try_from(*offset).map_err(|_| address_overflow())?).map(Some)
        }
        TypedExpr::Unary { op: UnaryOp::Dereference, operand, .. } => generate(gen, operand),
        _ => Ok(None),
    }
}

fn add_cells(value: Value, cells: i64) -> Result<Value, CompilerError> {
    if cells == 0 { return Ok(value); }
    let Value::FatPtr(mut pointer) = value else {
        return Err(CodegenError::UnsupportedConstruct {
            construct: "arithmetic on a non-object address constant".into(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }.into());
    };
    let (base, cells) = match *pointer.addr {
        Value::AddressOffset { base, cells: previous } =>
            (base, previous.checked_add(cells).ok_or_else(address_overflow)?),
        base @ (Value::Global(_) | Value::Constant(_)) => (Box::new(base), cells),
        _ => return Err(address_overflow()),
    };
    pointer.addr = if cells == 0 { base } else { Box::new(Value::AddressOffset { base, cells }) };
    Ok(Value::FatPtr(pointer))
}

fn address_overflow() -> CompilerError {
    CodegenError::UnsupportedConstruct {
        construct: "static address displacement is not representable".into(),
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }.into()
}
