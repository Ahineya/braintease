//! Expression AST nodes for C99
//! 
//! This module defines expression nodes in the abstract syntax tree.

use crate::types::Type;
use super::ops::{BinaryOp, UnaryOp};
use crate::ast::NodeId;
use crate::lexer::IntegerSuffix;
use rcc_common::{SourceSpan, SymbolId};
use serde::{Deserialize, Serialize};

/// AST Expression nodes
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Expression {
    pub node_id: NodeId,
    pub kind: ExpressionKind,
    pub span: SourceSpan,
    pub expr_type: Option<Type>, // Filled during semantic analysis
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum ExpressionKind {
    /// Integer literal
    IntLiteral {
        value: i64,
        suffix: IntegerSuffix,
        hex: bool,
    },
    
    /// Character literal
    CharLiteral(u8),
    
    /// String literal
    StringLiteral(String),
    
    /// Identifier reference
    Identifier {
        name: String,
        symbol_id: Option<SymbolId>, // Filled during semantic analysis
    },
    
    /// Binary operation
    Binary {
        op: BinaryOp,
        left: Box<Expression>,
        right: Box<Expression>,
    },
    
    /// Unary operation
    Unary {
        op: UnaryOp,
        operand: Box<Expression>,
    },
    
    /// Function call
    Call {
        function: Box<Expression>,
        arguments: Vec<Expression>,
    },
    
    /// Array/struct member access
    Member {
        object: Box<Expression>,
        member: String,
        is_pointer: bool, // true for ->, false for .
    },
    
    /// Ternary conditional operator (condition ? then_expr : else_expr)
    Conditional {
        condition: Box<Expression>,
        then_expr: Box<Expression>,
        else_expr: Box<Expression>,
    },
    
    /// Type cast
    Cast {
        target_type: Type,
        operand: Box<Expression>,
    },
    
    /// Sizeof expression
    SizeofExpr(Box<Expression>),
    
    /// Sizeof type
    SizeofType(Type),
    
    /// Compound literal (C99)
    CompoundLiteral {
        type_name: Type,
        initializer: Box<Initializer>,
    },

    /// `__builtin_va_start(ap, last)` — assigns the first unnamed argument address to `ap`.
    VaStart {
        ap: Box<Expression>,
        last: Box<Expression>,
    },

    /// `__builtin_va_arg(ap, type)` — yields the next unnamed argument and advances `ap`.
    VaArg {
        ap: Box<Expression>,
        arg_type: Type,
    },
}

/// Initializer for variables, arrays, structs
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Initializer {
    pub node_id: NodeId,
    pub kind: InitializerKind,
    pub span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum InitializerKind {
    /// Single expression
    Expression(Expression),
    
    /// Initializer list (for arrays, structs)
    List(Vec<Initializer>),
    
    /// Designated initializer (C99): .field = value or [index] = value
    Designated {
        designator: Designator,
        initializer: Box<Initializer>,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Designator {
    /// Array index: [index]
    Index(Expression),
    
    /// Struct member: .member
    Member(String),
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_common::SourceLocation;

    #[test]
    fn test_expression_creation() {
        let loc = SourceLocation::new_simple(1, 1);
        let span = SourceSpan::new(loc.clone(), loc);
        
        let expr = Expression {
            node_id: 0,
            kind: ExpressionKind::IntLiteral {
                value: 42,
                suffix: IntegerSuffix::None,
                hex: false,
            },
            span: span.clone(),
            expr_type: Some(Type::Int),
        };
        
        assert_eq!(expr.node_id, 0);
        match expr.kind {
            ExpressionKind::IntLiteral { value, .. } => assert_eq!(value, 42),
            _ => panic!("Expected IntLiteral"),
        }
        assert_eq!(expr.expr_type, Some(Type::Int));
    }
}