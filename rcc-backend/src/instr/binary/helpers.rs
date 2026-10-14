//! Helper functions for binary operation lowering

use rcc_ir::ir::{Value, IrBinaryOp};
use crate::regmgmt::RegisterPressureManager;
use crate::instr::helpers::calculate_value_need;

/// Calculate register needs for operands
pub(super) fn calculate_register_needs(
    _mgr: &RegisterPressureManager,
    lhs: &Value,
    rhs: &Value,
) -> (usize, usize) {
    let left_need = calculate_value_need(lhs);
    let right_need = calculate_value_need(rhs);
    (left_need, right_need)
}

/// Check if operation is commutative
pub(super) fn is_commutative(op: IrBinaryOp) -> bool {
    matches!(op, 
        IrBinaryOp::Add | 
        IrBinaryOp::Mul | 
        IrBinaryOp::And | 
        IrBinaryOp::Or | 
        IrBinaryOp::Xor |
        IrBinaryOp::Eq |  // Equality is commutative
        IrBinaryOp::Ne    // Not-equal is commutative
    )
}

/// Check if we can reuse the first operand's register for the result
pub(super) fn can_reuse_register(_op: IrBinaryOp) -> bool {
    // Most operations can reuse the destination register
    // Some architectures may have restrictions
    true
}