//! Statement AST nodes for C99
//! 
//! This module defines statement nodes and function definitions.

use crate::types::{Type, StorageClass};
use super::expressions::{Expression, Initializer};
use crate::ast::NodeId;
use rcc_common::{SourceSpan, SymbolId};
use serde::{Deserialize, Serialize};

/// Inline assembly operand
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AsmOperand {
    pub constraint: String,  // e.g., "=r" for output, "r" for input, "+r" for input/output
    pub expr: Expression,    // The C expression tied to this operand
}

/// AST Statement nodes
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Statement {
    pub node_id: NodeId,
    pub kind: StatementKind,
    pub span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum StatementKind {
    /// Expression statement
    Expression(Expression),
    
    /// Compound statement (block)
    Compound(Vec<Statement>),
    
    /// Variable declaration
    Declaration {
        declarations: Vec<Declaration>,
    },
    
    /// If statement
    If {
        condition: Expression,
        then_stmt: Box<Statement>,
        else_stmt: Option<Box<Statement>>,
    },
    
    /// While loop
    While {
        condition: Expression,
        body: Box<Statement>,
    },
    
    /// For loop
    For {
        init: Option<Box<Statement>>, // Can be declaration or expression
        condition: Option<Expression>,
        update: Option<Expression>,
        body: Box<Statement>,
    },
    
    /// Do-while loop
    DoWhile {
        body: Box<Statement>,
        condition: Expression,
    },
    
    /// Switch statement
    Switch {
        expression: Expression,
        body: Box<Statement>,
    },
    
    /// Case label
    Case {
        value: Expression,
        statement: Box<Statement>,
    },
    
    /// Default case
    Default {
        statement: Box<Statement>,
    },
    
    /// Break statement
    Break,
    
    /// Continue statement
    Continue,
    
    /// Return statement
    Return(Option<Expression>),
    
    /// Goto statement
    Goto(String),
    
    /// Label statement
    Label {
        name: String,
        statement: Box<Statement>,
    },
    
    /// Inline assembly
    InlineAsm {
        assembly: String,           // Raw assembly code
        outputs: Vec<AsmOperand>,   // Output operands
        inputs: Vec<AsmOperand>,    // Input operands  
        clobbers: Vec<String>,      // Clobbered registers
    },
    
    /// Empty statement (just semicolon)
    Empty,
}

/// Variable/function declaration
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Declaration {
    pub node_id: NodeId,
    pub name: String,
    pub decl_type: Type,
    pub storage_class: StorageClass,
    pub initializer: Option<Initializer>,
    pub span: SourceSpan,
    pub symbol_id: Option<SymbolId>, // Filled during semantic analysis
}

/// Function definition
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FunctionDefinition {
    pub node_id: NodeId,
    pub name: String,
    pub return_type: Type,
    pub parameters: Vec<Parameter>,
    pub body: Statement,
    pub storage_class: StorageClass,
    pub is_variadic: bool,
    pub span: SourceSpan,
    pub symbol_id: Option<SymbolId>, // Filled during semantic analysis
}

/// Function parameter
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Parameter {
    pub node_id: NodeId,
    pub name: Option<String>, // Can be unnamed in function prototypes
    pub param_type: Type,
    pub span: SourceSpan,
    pub symbol_id: Option<SymbolId>, // Filled during semantic analysis
}

/// Top-level compilation unit
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TranslationUnit {
    pub node_id: NodeId,
    pub items: Vec<TopLevelItem>,
    pub span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum TopLevelItem {
    /// Function definition
    Function(FunctionDefinition),
    
    /// Global variable declarations (can be multiple with comma-separated declarators)
    Declarations(Vec<Declaration>),
    
    /// Struct/union/enum definition
    TypeDefinition {
        name: String,
        type_def: Type,
        span: SourceSpan,
    },
}