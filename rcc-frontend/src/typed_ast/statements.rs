//! Typed statements
//!
//! This module defines typed statements produced by semantic analysis.

use super::expressions::TypedExpr;
use crate::types::Type;
use rcc_common::SymbolId;
use serde::{Deserialize, Serialize};

/// Typed inline assembly operand
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TypedAsmOperand {
    pub constraint: String,
    pub expr: TypedExpr,
}

/// Typed statement - produced by semantic analysis
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum TypedStmt {
    /// Expression statement
    Expression(TypedExpr),
    
    /// Compound statement (block)
    Compound(Vec<TypedStmt>),
    
    /// Variable declaration
    Declaration {
        name: String,
        decl_type: Type,
        initializer: Option<TypedExpr>,
        symbol_id: Option<SymbolId>,
    },
    
    /// If statement
    If {
        condition: TypedExpr,
        then_stmt: Box<TypedStmt>,
        else_stmt: Option<Box<TypedStmt>>,
    },
    
    /// While loop
    While {
        condition: TypedExpr,
        body: Box<TypedStmt>,
    },
    
    /// For loop
    For {
        init: Option<Box<TypedStmt>>,
        condition: Option<TypedExpr>,
        update: Option<TypedExpr>,
        body: Box<TypedStmt>,
    },

    /// Switch statement
    Switch {
        expression: TypedExpr,
        body: Box<TypedStmt>,
    },

    /// Do-while loop
    DoWhile {
        condition: TypedExpr,
        body: Box<TypedStmt>,
    },

    /// Case label (value is a folded integer constant)
    Case {
        value: i64,
        statement: Box<TypedStmt>,
    },

    /// Default label
    Default {
        statement: Box<TypedStmt>,
    },
    
    /// Return statement
    Return(Option<TypedExpr>),
    
    /// Break statement
    Break,
    
    /// Continue statement
    Continue,
    
    /// Inline assembly
    InlineAsm {
        assembly: String,
        outputs: Vec<TypedAsmOperand>,
        inputs: Vec<TypedAsmOperand>,
        clobbers: Vec<String>,
    },
    
    /// Empty statement
    Empty,
}