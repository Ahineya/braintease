//! Binary operation code generation

use super::{TypedExpressionGenerator, convert_type_default};
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
/// Convert each operand to a boolean (`!= 0`) first, then AND/OR those 0/1
/// values. Both sides are still evaluated (no extra blocks) so loop-condition
/// bank bindings stay in one block.
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

fn generate_logical_operation(
    gen: &mut TypedExpressionGenerator,
    op: BinaryOp,
    left: &TypedExpr,
    right: &TypedExpr,
) -> Result<Value, CompilerError> {
    let left_val = gen.generate(left)?;
    let left_bool = to_logical_i16(gen, left_val)?;
    let right_val = gen.generate(right)?;
    let right_bool = to_logical_i16(gen, right_val)?;
    let ir_op = match op {
        BinaryOp::LogicalAnd => IrBinaryOp::And,
        BinaryOp::LogicalOr => IrBinaryOp::Or,
        _ => unreachable!("generate_logical_operation only handles && / ||"),
    };
    let result = gen.builder.build_binary(ir_op, left_bool, right_bool, IrType::I16)?;
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

    let left_val = gen.generate(left)?;
    let right_val = gen.generate(right)?;
    let ir_type = convert_type_default(result_type)?;
    
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
    gen.builder.build_store(Value::Temp(result), lhs_addr)?;
    
    Ok(Value::Temp(result))
}