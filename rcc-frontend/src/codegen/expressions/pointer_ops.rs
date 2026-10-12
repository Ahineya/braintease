//! Pointer operation code generation

use super::{TypedExpressionGenerator, convert_type_default};
use crate::ast::BinaryOp;
use crate::ir::{FatPointer, IrBinaryOp, IrUnaryOp, IrType, Value};
use crate::typed_ast::TypedExpr;
use crate::types::{BankTag, Type};
use crate::codegen::CodegenError;
use crate::CompilerError;

pub(super) fn widen_index(gen: &mut TypedExpressionGenerator, value: Value, ty: &Type) -> Result<Value, CompilerError> {
    // Keep compile-time displacements visible to the native constant GEP path.
    if matches!(value, Value::Constant(_)) { return Ok(value); }
    super::conversions::convert_integer(gen, value, ty, &Type::Long)
}

pub fn generate_pointer_arithmetic(
    gen: &mut TypedExpressionGenerator,
    ptr: &TypedExpr,
    offset: &TypedExpr,
    _elem_type: &Type,
    is_add: bool,
    expr_type: &Type,
) -> Result<Value, CompilerError> {
    // THIS IS THE KEY PART: Generate GEP for pointer arithmetic!
    let ptr_val = gen.generate(ptr)?;
    let raw_offset = gen.generate(offset)?;
    let offset_val = widen_index(gen, raw_offset, offset.get_type())?;
    
    // A pointer to an array advances by a complete array. Array decay has
    // already happened in semantic analysis; do not decay its pointee again.
    let ir_type = convert_type_default(expr_type)?;
    
    // Handle add vs subtract
    let final_offset = if is_add {
        offset_val
    } else if let Value::Constant(n) = offset_val {
        Value::Constant(n.wrapping_neg())
    } else {
        // For subtraction, negate the offset
        let neg_offset = gen.builder.build_binary(
            IrBinaryOp::Sub,
            Value::Constant(0),
            offset_val,
            IrType::I32,
        )?;
        Value::Temp(neg_offset)
    };
    
    // Generate GEP instruction - this handles bank overflow!
    let result = gen
        .builder
        .build_pointer_offset(ptr_val, final_offset, ir_type)?;
    
    Ok(result)
}

pub fn generate_pointer_difference(
    gen: &mut TypedExpressionGenerator,
    left: &TypedExpr,
    right: &TypedExpr,
    elem_type: &Type,
) -> Result<Value, CompilerError> {
    // Generate pointer difference (returns number of elements)
    let left_val = gen.generate(left)?;
    let right_val = gen.generate(right)?;
    
    // Linear cell addresses retain the bank when subtracting pointers.
    let left_addr = gen.builder.build_unary(IrUnaryOp::PtrToLinear, left_val, IrType::I32)?;
    let right_addr = gen.builder.build_unary(IrUnaryOp::PtrToLinear, right_val, IrType::I32)?;
    let diff = gen.builder.build_binary(IrBinaryOp::Sub,
        Value::Temp(left_addr), Value::Temp(right_addr), IrType::I32)?;
    let elem_size = elem_type.size_in_words().unwrap_or(1) as i64;
    if elem_size == 1 {
        return Ok(Value::Temp(diff));
    }
    let result = gen.builder.build_binary(IrBinaryOp::SDiv,
        Value::Temp(diff), Value::Constant(elem_size), IrType::I32)?;

    Ok(Value::Temp(result))
}

pub fn generate_array_index(
    gen: &mut TypedExpressionGenerator,
    array: &TypedExpr,
    index: &TypedExpr,
    elem_type: &Type,
) -> Result<Value, CompilerError> {
    // Array indexing is pointer arithmetic followed by load (sometimes)
    let array_val = gen.generate(array)?;
    let raw_index = gen.generate(index)?;
    let index_val = widen_index(gen, raw_index, index.get_type())?;
    
    // Generate GEP for the element address
    let elem_ptr_type = convert_type_default(&Type::Pointer {
        target: Box::new(elem_type.clone()),
        bank: None,
    })?;
    
    let elem_ptr =
        gen.builder
            .build_pointer_offset(array_val, index_val, elem_ptr_type)?;
    
    // Arrays decay to pointers. Aggregates are used as pointers (member
    // access, assignment, byval args) so do not load them as scalars.
    if matches!(elem_type.unqualified(), Type::Array { .. }) || elem_type.is_aggregate() {
        Ok(elem_ptr)
    } else {
        let elem_ir_type = convert_type_default(elem_type)?;
        let result = gen.builder.build_load_volatile(elem_ptr, elem_ir_type, elem_type.is_volatile())?;
        if matches!(elem_type.unqualified(), Type::Pointer { .. }) {
            Ok(Value::FatPtr(FatPointer {
                addr: Box::new(Value::Temp(result)),
                bank: BankTag::Mixed,
            }))
        } else {
            Ok(Value::Temp(result))
        }
    }
}

pub fn generate_pointer_compound_assignment(
    gen: &mut TypedExpressionGenerator,
    op: BinaryOp,
    lhs: &TypedExpr,
    rhs: &TypedExpr,
) -> Result<Value, CompilerError> {
    use super::unary_ops::generate_lvalue_address;
    
    // For pointer compound assignment (p += n), use GEP
    let lhs_addr = generate_lvalue_address(gen, lhs)?;
    let lhs_val = {
        let ir_type = convert_type_default(lhs.get_type())?;
        let temp = gen.builder.build_load_volatile(lhs_addr.clone(), ir_type, lhs.get_declared_type().is_volatile())?;
        // A pointer load carries both runtime components. Preserve that value
        // shape for GEP just as identifier loads and pointer increments do;
        // the bank of the lvalue's storage is not the pointee's bank.
        Value::FatPtr(FatPointer {
            addr: Box::new(Value::Temp(temp)),
            bank: BankTag::Mixed,
        })
    };
    let raw_rhs = gen.generate(rhs)?;
    let rhs_val = widen_index(gen, raw_rhs, rhs.get_type())?;
    
    // Handle add vs subtract
    let final_offset = match op {
        BinaryOp::AddAssign => rhs_val,
        BinaryOp::SubAssign => {
            if let Value::Constant(n) = rhs_val {
                Value::Constant(n.wrapping_neg())
            } else {
            // Negate for subtraction
            let neg = gen.builder.build_binary(
                IrBinaryOp::Sub,
                Value::Constant(0),
                rhs_val,
                IrType::I32,
            )?;
            Value::Temp(neg)
            }
        }
        _ => {
            return Err(CodegenError::UnsupportedConstruct {
                construct: format!("pointer compound assignment: {op:?}"),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            }
            .into())
        }
    };
    
    // Generate GEP for pointer arithmetic
    let ir_type = convert_type_default(lhs.get_type())?;
    let result = gen
        .builder
        .build_pointer_offset(lhs_val, final_offset, ir_type.clone())?;
    
    gen.builder.build_store_volatile(result.clone(), lhs_addr, ir_type, lhs.get_declared_type().is_volatile())?;
    Ok(result)
}
/// Integer representation of a pointer includes both its offset and bank.
pub fn pointer_integer(gen: &mut TypedExpressionGenerator, value: Value) -> Result<Value, CompilerError> {
    if matches!(value, Value::Constant(0)) { return Ok(value); }
    let id = gen.builder.build_unary(IrUnaryOp::PtrToInt, value, IrType::I32)?;
    Ok(Value::Temp(id))
}

pub fn generate_pointer_comparison(gen: &mut TypedExpressionGenerator, op: BinaryOp,
    left: &TypedExpr, right: &TypedExpr) -> Result<Value, CompilerError> {
    let lv = gen.generate(left)?;
    let rv = gen.generate(right)?;
    let lv = pointer_integer(gen, lv)?;
    let rv = pointer_integer(gen, rv)?;
    let op = match op {
        BinaryOp::Equal => IrBinaryOp::Eq, BinaryOp::NotEqual => IrBinaryOp::Ne,
        BinaryOp::Less => IrBinaryOp::Ult, BinaryOp::LessEqual => IrBinaryOp::Ule,
        BinaryOp::Greater => IrBinaryOp::Ugt, BinaryOp::GreaterEqual => IrBinaryOp::Uge,
        _ => unreachable!("pointer comparison"),
    };
    Ok(Value::Temp(gen.builder.build_binary(op, lv, rv, IrType::I16)?))
}
