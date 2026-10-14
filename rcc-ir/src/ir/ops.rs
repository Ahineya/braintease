//! IR Operations
//! 
//! Defines binary and unary operations available in the IR.

use serde::{Deserialize, Serialize};
use std::fmt;

/// Binary operations in IR
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum IrBinaryOp {
    // Arithmetic
    Add, Sub, Mul, 
    SDiv, UDiv,    // Signed/unsigned division
    SRem, URem,    // Signed/unsigned remainder
    
    // Bitwise
    And, Or, Xor,
    Shl, LShr, AShr, // Logical/arithmetic shift right
    
    // Comparison (return i1)
    Eq, Ne,
    Slt, Sle, Sgt, Sge, // Signed comparisons
    Ult, Ule, Ugt, Uge, // Unsigned comparisons
}

impl fmt::Display for IrBinaryOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let op_str = match self {
            IrBinaryOp::Add => "add",
            IrBinaryOp::Sub => "sub",
            IrBinaryOp::Mul => "mul",
            IrBinaryOp::SDiv => "sdiv",
            IrBinaryOp::UDiv => "udiv",
            IrBinaryOp::SRem => "srem",
            IrBinaryOp::URem => "urem",
            IrBinaryOp::And => "and",
            IrBinaryOp::Or => "or",
            IrBinaryOp::Xor => "xor",
            IrBinaryOp::Shl => "shl",
            IrBinaryOp::LShr => "lshr",
            IrBinaryOp::AShr => "ashr",
            IrBinaryOp::Eq => "eq",
            IrBinaryOp::Ne => "ne",
            IrBinaryOp::Slt => "slt",
            IrBinaryOp::Sle => "sle",
            IrBinaryOp::Sgt => "sgt",
            IrBinaryOp::Sge => "sge",
            IrBinaryOp::Ult => "ult",
            IrBinaryOp::Ule => "ule",
            IrBinaryOp::Ugt => "ugt",
            IrBinaryOp::Uge => "uge",
        };
        write!(f, "{op_str}")
    }
}

/// Unary operations in IR
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum IrUnaryOp {
    Not,     // Bitwise NOT
    Neg,     // Arithmetic negation
    ZExt,    // Zero extend
    SExt,    // Sign extend
    Trunc,   // Truncate
    PtrToLinear, // Linear cell address, for bank-aware pointer subtraction
    PtrToInt, // Pointer to integer cast
    IntToPtr, // Integer to pointer cast
}

impl fmt::Display for IrUnaryOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let op_str = match self {
            IrUnaryOp::Not => "not",
            IrUnaryOp::Neg => "neg",
            IrUnaryOp::ZExt => "zext",
            IrUnaryOp::SExt => "sext",
            IrUnaryOp::Trunc => "trunc",
            IrUnaryOp::PtrToLinear => "ptrtolinear",
            IrUnaryOp::PtrToInt => "ptrtoint",
            IrUnaryOp::IntToPtr => "inttoptr",
        };
        write!(f, "{op_str}")
    }
}