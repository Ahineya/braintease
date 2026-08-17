//! Typed expressions
//!
//! This module defines typed expressions that make type-dependent operations explicit.

use crate::types::Type;
use crate::ast::{BinaryOp, UnaryOp};
use rcc_common::SymbolId;
use serde::{Deserialize, Serialize};

/// Typed expression - produced by semantic analysis
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum TypedExpr {
    /// Integer literal
    IntLiteral {
        value: i64,
        expr_type: Type,
    },
    
    /// Character literal
    CharLiteral {
        value: u8,
        expr_type: Type,
    },
    
    /// String literal
    StringLiteral {
        value: String,
        expr_type: Type,
    },
    
    /// Variable reference
    Variable {
        name: String,
        symbol_id: Option<SymbolId>,
        expr_type: Type,
    },
    
    /// Regular binary operation (both operands same type)
    Binary {
        op: BinaryOp,
        left: Box<TypedExpr>,
        right: Box<TypedExpr>,
        expr_type: Type,
    },
    
    /// Pointer arithmetic: ptr + integer or ptr - integer
    PointerArithmetic {
        ptr: Box<TypedExpr>,
        offset: Box<TypedExpr>,
        elem_type: Type,
        is_add: bool,  // true for add, false for subtract
        expr_type: Type,  // Result type (pointer)
    },
    
    /// Pointer difference: ptr - ptr (returns integer)
    PointerDifference {
        left: Box<TypedExpr>,
        right: Box<TypedExpr>,
        elem_type: Type,
        expr_type: Type,  // Result type (integer)
    },
    
    /// Array indexing: arr[idx]
    ArrayIndex {
        array: Box<TypedExpr>,
        index: Box<TypedExpr>,
        elem_type: Type,
        expr_type: Type,  // Element type
    },
    
    /// Struct/union member access
    MemberAccess {
        object: Box<TypedExpr>,
        member: String,
        offset: u64,  // Computed offset in words
        is_pointer: bool,  // true for ->, false for .
        expr_type: Type,
    },
    
    /// Unary operation
    Unary {
        op: UnaryOp,
        operand: Box<TypedExpr>,
        expr_type: Type,
    },
    
    /// Function call
    Call {
        function: Box<TypedExpr>,
        arguments: Vec<TypedExpr>,
        /// If this call is to a variadic function, arguments at this index
        /// and beyond are unnamed extras and must be passed on the stack.
        stack_args_from: Option<usize>,
        expr_type: Type,
    },
    
    /// Type cast
    Cast {
        operand: Box<TypedExpr>,
        target_type: Type,
        expr_type: Type,
    },
    
    /// Conditional expression (? :)
    Conditional {
        condition: Box<TypedExpr>,
        then_expr: Box<TypedExpr>,
        else_expr: Box<TypedExpr>,
        expr_type: Type,
    },
    
    /// Assignment (returns assigned value)
    Assignment {
        lhs: Box<TypedExpr>,
        rhs: Box<TypedExpr>,
        expr_type: Type,
    },
    
    /// Compound assignment (+=, -=, etc.)
    CompoundAssignment {
        op: BinaryOp,
        lhs: Box<TypedExpr>,
        rhs: Box<TypedExpr>,
        is_pointer: bool,  // true if LHS is pointer
        expr_type: Type,
    },
    
    /// Sizeof expression
    SizeofExpr {
        operand: Box<TypedExpr>,
        expr_type: Type,  // Always size_t/int
    },
    
    /// Sizeof type
    SizeofType {
        target_type: Type,
        expr_type: Type,  // Always size_t/int
    },
    
    /// Array initializer list
    ArrayInitializer {
        elements: Vec<TypedExpr>,
        expr_type: Type,  // Array type
    },
    
    /// Compound literal (C99)
    CompoundLiteral {
        initializer: Vec<TypedExpr>,  // Initialize elements
        expr_type: Type,  // The literal's type
    },

    /// `__builtin_va_start(ap, last)`
    VaStart {
        ap: Box<TypedExpr>,
        expr_type: Type,
    },

    /// `__builtin_va_arg(ap, type)`
    VaArg {
        ap: Box<TypedExpr>,
        arg_type: Type,
        expr_type: Type,
    },
}

impl TypedExpr {
    /// Get the type of this expression
    pub fn get_type(&self) -> &Type {
        match self {
            TypedExpr::IntLiteral { expr_type, .. } |
            TypedExpr::CharLiteral { expr_type, .. } |
            TypedExpr::StringLiteral { expr_type, .. } |
            TypedExpr::Variable { expr_type, .. } |
            TypedExpr::Binary { expr_type, .. } |
            TypedExpr::PointerArithmetic { expr_type, .. } |
            TypedExpr::PointerDifference { expr_type, .. } |
            TypedExpr::ArrayIndex { expr_type, .. } |
            TypedExpr::MemberAccess { expr_type, .. } |
            TypedExpr::Unary { expr_type, .. } |
            TypedExpr::Call { expr_type, .. } |
            TypedExpr::Cast { expr_type, .. } |
            TypedExpr::Conditional { expr_type, .. } |
            TypedExpr::Assignment { expr_type, .. } |
            TypedExpr::CompoundAssignment { expr_type, .. } |
            TypedExpr::SizeofExpr { expr_type, .. } |
            TypedExpr::SizeofType { expr_type, .. } |
            TypedExpr::ArrayInitializer { expr_type, .. } |
            TypedExpr::CompoundLiteral { expr_type, .. } |
            TypedExpr::VaStart { expr_type, .. } |
            TypedExpr::VaArg { expr_type, .. } => expr_type,
        }
    }
}