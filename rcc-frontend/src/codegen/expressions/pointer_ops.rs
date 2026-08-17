//! Pointer operation code generation

use super::{TypedExpressionGenerator, convert_type_default};
use crate::ast::BinaryOp;
use crate::ir::{IrBinaryOp, IrType, Value};
use crate::typed_ast::TypedExpr;
use crate::types::Type;
use crate::codegen::CodegenError;
use crate::CompilerError;

pub fn generate_pointer_arithmetic(
    gen: &mut TypedExpressionGenerator,
    ptr: &TypedExpr,
    offset: &TypedExpr,
    elem_type: &Type,
    is_add: bool,
    expr_type: &Type,
) -> Result<Value, CompilerError> {
    // THIS IS THE KEY PART: Generate GEP for pointer arithmetic!
    let ptr_val = gen.generate(ptr)?;
    let offset_val = gen.generate(offset)?;
    
    // CRITICAL FIX: Check if we're doing arithmetic on a pointer to an array.
    // If so, we need to handle array decay - the array should decay to a pointer
    // to its first element for arithmetic purposes.
    let ir_type = if let Type::Array { element_type, .. } = elem_type {
        // Array decay: when doing arithmetic on a pointer to an array,
        // treat it as arithmetic on a pointer to the array's element type
        convert_type_default(&Type::Pointer {
            target: element_type.clone(),
            bank: None,
        })?
    } else {
        // Normal case: use the expr_type from semantic analysis
        convert_type_default(expr_type)?
    };
    
    // Handle add vs subtract
    let final_offset = if is_add {
        offset_val
    } else {
        // For subtraction, negate the offset
        let neg_offset = gen.builder.build_binary(
            IrBinaryOp::Sub,
            Value::Constant(0),
            offset_val,
            IrType::I16,
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
    
    // Extract address components from FatPointers
    // When subtracting pointers, we only care about addresses, not banks
    let left_addr = match left_val {
        Value::FatPtr(ref fp) => *fp.addr.clone(),
        _ => left_val.clone(),
    };
    
    let right_addr = match right_val {
        Value::FatPtr(ref fp) => *fp.addr.clone(),
        _ => right_val.clone(),
    };
    
    // Calculate byte difference using only addresses
    let byte_diff =
        gen.builder
            .build_binary(IrBinaryOp::Sub, left_addr, right_addr, IrType::I16)?;
    
    // Divide by element size to get element count
    // Use size_in_words since Ripple VM is word-addressed
    let elem_size = elem_type.size_in_words().unwrap_or(1) as i64;
    let result = gen.builder.build_binary(
        IrBinaryOp::UDiv,
        Value::Temp(byte_diff),
        Value::Constant(elem_size),
        IrType::I16,
    )?;
    
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
    let index_val = gen.generate(index)?;
    
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
    if matches!(elem_type, Type::Array { .. }) || elem_type.is_aggregate() {
        Ok(elem_ptr)
    } else {
        let elem_ir_type = convert_type_default(elem_type)?;
        let result = gen.builder.build_load(elem_ptr, elem_ir_type)?;
        Ok(Value::Temp(result))
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
        let temp = gen.builder.build_load(lhs_addr.clone(), ir_type)?;
        Value::Temp(temp)
    };
    let rhs_val = gen.generate(rhs)?;
    
    // Handle add vs subtract
    let final_offset = match op {
        BinaryOp::AddAssign => rhs_val,
        BinaryOp::SubAssign => {
            // Negate for subtraction
            let neg = gen.builder.build_binary(
                IrBinaryOp::Sub,
                Value::Constant(0),
                rhs_val,
                IrType::I16,
            )?;
            Value::Temp(neg)
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
    
    gen.builder.build_store(result.clone(), lhs_addr, ir_type)?;
    Ok(result)
}