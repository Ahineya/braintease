//! Operator definitions for C99
//! 
//! This module defines binary and unary operators used in expressions.

use serde::{Deserialize, Serialize};
use std::fmt;

/// Binary operators
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum BinaryOp {
    // Arithmetic
    Add, Sub, Mul, Div, Mod,
    
    // Bitwise
    BitAnd, BitOr, BitXor, LeftShift, RightShift,
    
    // Logical
    LogicalAnd, LogicalOr,
    
    // Comparison
    Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
    
    // Assignment
    Assign,
    AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
    BitAndAssign, BitOrAssign, BitXorAssign, LeftShiftAssign, RightShiftAssign,
    
    // Array/pointer access
    Index,

    // Sequence
    Comma,
}

impl fmt::Display for BinaryOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let op_str = match self {
            BinaryOp::Add => "+",
            BinaryOp::Sub => "-",
            BinaryOp::Mul => "*",
            BinaryOp::Div => "/",
            BinaryOp::Mod => "%",
            BinaryOp::BitAnd => "&",
            BinaryOp::BitOr => "|",
            BinaryOp::BitXor => "^",
            BinaryOp::LeftShift => "<<",
            BinaryOp::RightShift => ">>",
            BinaryOp::LogicalAnd => "&&",
            BinaryOp::LogicalOr => "||",
            BinaryOp::Equal => "==",
            BinaryOp::NotEqual => "!=",
            BinaryOp::Less => "<",
            BinaryOp::Greater => ">",
            BinaryOp::LessEqual => "<=",
            BinaryOp::GreaterEqual => ">=",
            BinaryOp::Assign => "=",
            BinaryOp::AddAssign => "+=",
            BinaryOp::SubAssign => "-=",
            BinaryOp::MulAssign => "*=",
            BinaryOp::DivAssign => "/=",
            BinaryOp::ModAssign => "%=",
            BinaryOp::BitAndAssign => "&=",
            BinaryOp::BitOrAssign => "|=",
            BinaryOp::BitXorAssign => "^=",
            BinaryOp::LeftShiftAssign => "<<=",
            BinaryOp::RightShiftAssign => ">>=",
            BinaryOp::Index => "[]",
            BinaryOp::Comma => ",",
        };
        write!(f, "{op_str}")
    }
}

/// Unary operators
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum UnaryOp {
    // Arithmetic
    Plus, Minus,
    
    // Bitwise
    BitNot,
    
    // Logical
    LogicalNot,
    
    // Pointer/address
    Dereference, AddressOf,
    
    // Pre/post increment/decrement
    PreIncrement, PostIncrement,
    PreDecrement, PostDecrement,
    
    // Sizeof
    Sizeof,
}

impl fmt::Display for UnaryOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let op_str = match self {
            UnaryOp::Plus => "+",
            UnaryOp::Minus => "-",
            UnaryOp::BitNot => "~",
            UnaryOp::LogicalNot => "!",
            UnaryOp::Dereference => "*",
            UnaryOp::AddressOf => "&",
            UnaryOp::PreIncrement => "++",
            UnaryOp::PostIncrement => "++",
            UnaryOp::PreDecrement => "--",
            UnaryOp::PostDecrement => "--",
            UnaryOp::Sizeof => "sizeof",
        };
        write!(f, "{op_str}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_binary_op_display() {
        assert_eq!(format!("{}", BinaryOp::Add), "+");
        assert_eq!(format!("{}", BinaryOp::Equal), "==");
        assert_eq!(format!("{}", BinaryOp::LogicalAnd), "&&");
        assert_eq!(format!("{}", BinaryOp::Index), "[]");
    }

    #[test]
    fn test_unary_op_display() {
        assert_eq!(format!("{}", UnaryOp::Minus), "-");
        assert_eq!(format!("{}", UnaryOp::Dereference), "*");
        assert_eq!(format!("{}", UnaryOp::LogicalNot), "!");
        assert_eq!(format!("{}", UnaryOp::Sizeof), "sizeof");
    }
}