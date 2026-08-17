//! Abstract Syntax Tree definitions for C99
//! 
//! This module defines the AST nodes that represent C99 language constructs.
//! The AST is built by the parser and used by semantic analysis and IR generation.

pub mod ops;
pub mod expressions;
pub mod statements;
pub mod const_eval;

// Re-export commonly used types at module level
pub use ops::{BinaryOp, UnaryOp};
pub use expressions::{Expression, ExpressionKind, Initializer, InitializerKind, Designator};
pub use statements::{
    Statement, StatementKind, Declaration, FunctionDefinition, 
    Parameter, TranslationUnit, TopLevelItem
};

/// Unique identifier for AST nodes (useful for debugging and analysis)
pub type NodeId = u32;

/// Node ID generator for AST nodes
#[derive(Debug, Clone, Default)]
pub struct NodeIdGenerator {
    next_id: NodeId,
}

impl NodeIdGenerator {
    pub fn new() -> Self {
        Self { next_id: 0 }
    }
    
    pub fn next(&mut self) -> NodeId {
        let id = self.next_id;
        self.next_id += 1;
        id
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_node_id_generator() {
        let mut gen = NodeIdGenerator::new();
        assert_eq!(gen.next(), 0);
        assert_eq!(gen.next(), 1);
        assert_eq!(gen.next(), 2);
    }
}