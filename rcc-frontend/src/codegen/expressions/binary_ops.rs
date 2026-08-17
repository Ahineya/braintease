//! Binary operation code generation

use super::{TypedExpressionGenerator, convert_type_default};
use super::conversions::{convert_integer, usual_arithmetic_type};
use crate::ast::BinaryOp;
use crate::ir::{IrBinaryOp, IrType, Value};
use crate::typed_ast::TypedExpr;
use crate::types::Type;
use crate::codegen::CodegenError;
use crate::CompilerError;

/// C usual arithmetic conversions on this 16-bit target: if either operand is
/// unsigned, the operation is unsigned. Comparison result type is always `int`,
/// so signedness must come from the operands, not `result_type`.
fn operands_are_unsigned(left: &TypedExpr, right: &TypedExpr) -> bool {
    left.get_type().is_unsigned_integer() || right.get_type().is_unsigned_integer()
}

fn select_ir_op(op: BinaryOp, left: &TypedExpr, right: &TypedExpr) -> Result<IrBinaryOp, CompilerError> {
    let unsigned = operands_are_unsigned(left, right);
    // Right shift signedness follows the (promoted) left operand only.
    let left_unsigned = left.get_type().is_unsigned_integer();

    match op {
        BinaryOp::Add => Ok(IrBinaryOp::Add),
        BinaryOp::Sub => Ok(IrBinaryOp::Sub),
        BinaryOp::Mul => Ok(IrBinaryOp::Mul),
        BinaryOp::Div => Ok(if unsigned { IrBinaryOp::UDiv } else { IrBinaryOp::SDiv }),
        BinaryOp::Mod => Ok(if unsigned { IrBinaryOp::URem } else { IrBinaryOp::SRem }),
        BinaryOp::BitAnd => Ok(IrBinaryOp::And),
        BinaryOp::BitOr => Ok(IrBinaryOp::Or),
        BinaryOp::BitXor => Ok(IrBinaryOp::Xor),
        BinaryOp::LeftShift => Ok(IrBinaryOp::Shl),
        BinaryOp::RightShift => Ok(if left_unsigned { IrBinaryOp::LShr } else { IrBinaryOp::AShr }),
        BinaryOp::Less => Ok(if unsigned { IrBinaryOp::Ult } else { IrBinaryOp::Slt }),
        BinaryOp::Greater => Ok(if unsigned { IrBinaryOp::Ugt } else { IrBinaryOp::Sgt }),
        BinaryOp::LessEqual => Ok(if unsigned { IrBinaryOp::Ule } else { IrBinaryOp::Sle }),
        BinaryOp::GreaterEqual => Ok(if unsigned { IrBinaryOp::Uge } else { IrBinaryOp::Sge }),
        BinaryOp::Equal => Ok(IrBinaryOp::Eq),
        BinaryOp::NotEqual => Ok(IrBinaryOp::Ne),
        _ => Err(CodegenError::UnsupportedConstruct {
            construct: format!("binary op: {op:?}"),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }
        .into()),
    }
}

/// C logical && / || yield 0 or 1. Bitwise AND/OR of the raw operands is
/// wrong: `src[i] && i < 31` with src[i]=='B' (66) becomes `66 & 1 == 0`.
///
/// Convert each operand to a boolean (`!= 0`) first. Evaluation is
/// short-circuit: `&&` skips the RHS when the LHS is false, `||` skips
/// the RHS when the LHS is true. Side effects on the skipped side must
/// not run (C99 6.5.13 / 6.5.14).
fn scalar_for_logical(val: Value) -> Value {
    match val {
        Value::FatPtr(fp) => *fp.addr,
        other => other,
    }
}

fn to_logical_i16(gen: &mut TypedExpressionGenerator, val: Value) -> Result<Value, CompilerError> {
    let scalar = scalar_for_logical(val);
    let temp = gen.builder.build_binary(
        IrBinaryOp::Ne,
        scalar,
        Value::Constant(0),
        IrType::I16,
    )?;
    Ok(Value::Temp(temp))
}

fn intern_ir(result: Result<Value, String>) -> Result<Value, CompilerError> {
    result.map_err(|e| CodegenError::InternalError {
        message: e,
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }.into())
}

fn intern_unit(result: Result<(), String>) -> Result<(), CompilerError> {
    result.map_err(|e| CodegenError::InternalError {
        message: e,
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }.into())
}

fn intern_temp(result: Result<rcc_common::TempId, String>) -> Result<rcc_common::TempId, CompilerError> {
    result.map_err(|e| CodegenError::InternalError {
        message: e,
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }.into())
}

fn generate_logical_operation(
    gen: &mut TypedExpressionGenerator,
    op: BinaryOp,
    left: &TypedExpr,
    right: &TypedExpr,
) -> Result<Value, CompilerError> {
    let result_ptr = intern_ir(gen.builder.build_alloca(IrType::I16, None))?;

    let left_val = gen.generate(left)?;
    let left_bool = to_logical_i16(gen, left_val)?;

    let rhs_label = gen.builder.new_label();
    let skip_label = gen.builder.new_label();
    let end_label = gen.builder.new_label();

    match op {
        BinaryOp::LogicalAnd => {
            // If LHS is false, result is 0 and RHS is not evaluated.
            intern_unit(gen.builder.build_branch_cond(left_bool, rhs_label, skip_label))?;
        }
        BinaryOp::LogicalOr => {
            // If LHS is true, result is 1 and RHS is not evaluated.
            intern_unit(gen.builder.build_branch_cond(left_bool, skip_label, rhs_label))?;
        }
        _ => unreachable!("generate_logical_operation only handles && / ||"),
    }

    intern_unit(gen.builder.create_block(rhs_label).map(|_| ()))?;
    let right_val = gen.generate(right)?;
    let right_bool = to_logical_i16(gen, right_val)?;
        intern_unit(gen.builder.build_store(right_bool, result_ptr.clone(), IrType::I16))?;
    if !gen.builder.current_block_has_terminator() {
        intern_unit(gen.builder.build_branch(end_label))?;
    }

    intern_unit(gen.builder.create_block(skip_label).map(|_| ()))?;
    let skip_value = match op {
        BinaryOp::LogicalAnd => Value::Constant(0),
        BinaryOp::LogicalOr => Value::Constant(1),
        _ => unreachable!(),
    };
    intern_unit(gen.builder.build_store(skip_value, result_ptr.clone(), IrType::I16))?;
    if !gen.builder.current_block_has_terminator() {
        intern_unit(gen.builder.build_branch(end_label))?;
    }

    intern_unit(gen.builder.create_block(end_label).map(|_| ()))?;
    let result = intern_temp(gen.builder.build_load(result_ptr, IrType::I16))?;
    Ok(Value::Temp(result))
}

pub fn generate_binary_operation(
    gen: &mut TypedExpressionGenerator,
    op: BinaryOp,
    left: &TypedExpr,
    right: &TypedExpr,
    result_type: &Type,
) -> Result<Value, CompilerError> {
    if matches!(op, BinaryOp::LogicalAnd | BinaryOp::LogicalOr) {
        return generate_logical_operation(gen, op, left, right);
    }

    if op == BinaryOp::Comma {
        gen.generate(left)?;
        return gen.generate(right);
    }

    let left_val = gen.generate(left)?;
    let right_val = gen.generate(right)?;
    let is_cmp = matches!(
        op,
        BinaryOp::Less | BinaryOp::Greater | BinaryOp::LessEqual | BinaryOp::GreaterEqual
            | BinaryOp::Equal | BinaryOp::NotEqual
    );
    let is_shift = matches!(op, BinaryOp::LeftShift | BinaryOp::RightShift);
    let common = if is_cmp {
        usual_arithmetic_type(left.get_type(), right.get_type())
    } else if is_shift {
        // C99 6.5.7: result type is the promoted left operand; the count is not
        // converted to that type.
        result_type.clone()
    } else {
        result_type.clone()
    };
    let left_val = convert_integer(gen, left_val, left.get_type(), &common)?;
    let right_val = if is_shift {
        right_val
    } else {
        convert_integer(gen, right_val, right.get_type(), &common)?
    };
    let ir_type = convert_type_default(&common)?;
    
    let ir_op = select_ir_op(op, left, right)?;
    
    let result = gen
        .builder
        .build_binary(ir_op, left_val, right_val, ir_type)?;
    Ok(Value::Temp(result))
}

pub fn generate_compound_assignment(
    gen: &mut TypedExpressionGenerator,
    op: BinaryOp,
    lhs: &TypedExpr,
    rhs: &TypedExpr,
) -> Result<Value, CompilerError> {
    use super::unary_ops::generate_lvalue_address;
    
    let lhs_addr = generate_lvalue_address(gen, lhs)?;
    let lhs_val = {
        let ir_type = convert_type_default(lhs.get_type())?;
        let temp = gen.builder.build_load(lhs_addr.clone(), ir_type.clone())?;
        Value::Temp(temp)
    };
    let rhs_val = gen.generate(rhs)?;
    let rhs_val = convert_integer(gen, rhs_val, rhs.get_type(), lhs.get_type())?;
    
    let ir_type = convert_type_default(lhs.get_type())?;
    let unsigned = lhs.get_type().is_unsigned_integer() || rhs.get_type().is_unsigned_integer();
    let left_unsigned = lhs.get_type().is_unsigned_integer();
    
    let ir_op = match op {
        BinaryOp::AddAssign => IrBinaryOp::Add,
        BinaryOp::SubAssign => IrBinaryOp::Sub,
        BinaryOp::MulAssign => IrBinaryOp::Mul,
        BinaryOp::DivAssign => if unsigned { IrBinaryOp::UDiv } else { IrBinaryOp::SDiv },
        BinaryOp::ModAssign => if unsigned { IrBinaryOp::URem } else { IrBinaryOp::SRem },
        BinaryOp::BitAndAssign => IrBinaryOp::And,
        BinaryOp::BitOrAssign => IrBinaryOp::Or,
        BinaryOp::BitXorAssign => IrBinaryOp::Xor,
        BinaryOp::LeftShiftAssign => IrBinaryOp::Shl,
        BinaryOp::RightShiftAssign => if left_unsigned { IrBinaryOp::LShr } else { IrBinaryOp::AShr },
        _ => {
            return Err(CodegenError::UnsupportedConstruct {
                construct: format!("compound assignment: {op:?}"),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            }
            .into())
        }
    };
    
    let result = gen
        .builder
        .build_binary(ir_op, lhs_val, rhs_val, ir_type.clone())?;
    gen.builder.build_store(Value::Temp(result), lhs_addr, ir_type)?;
    
    Ok(Value::Temp(result))
}