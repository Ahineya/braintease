//! Ripple C99 Compiler - Frontend
//! 
//! This crate provides the frontend components for the Ripple C99 compiler:
//! - Lexer: tokenizes C99 source code
//! - Parser: builds AST from tokens
//! - AST: abstract syntax tree definitions
//! - Semantic analysis: type checking and symbol resolution (TODO)

pub mod lexer;
pub mod parser;
pub mod ast;
pub mod types;  // Type system definitions
pub mod semantic;
pub mod codegen;
pub mod ir;
pub mod typed_ast;
pub mod c_formatter;
mod codegen_tests;
mod type_environment_tests;
mod cast_tests;

use std::rc::Rc;
pub use lexer::{Lexer, Token, TokenType};
pub use parser::{Parser, ParseError};
pub use ast::{
    Expression, ExpressionKind, Statement, StatementKind,
    Declaration, FunctionDefinition, TranslationUnit, TopLevelItem,
    BinaryOp, UnaryOp, NodeIdGenerator
};
pub use types::{Type, BankTag, StructField, EnumVariant, StorageClass};
pub use semantic::{SemanticAnalyzer, SemanticError};
pub use codegen::{CodegenError, TypedCodeGenerator};
pub use typed_ast::{TypedTranslationUnit, type_translation_unit};

use rcc_common::CompilerError;
use crate::ir::Module;

/// High-level frontend interface
pub struct Frontend;

impl Frontend {
    /// Parse C99 source code into an AST
    pub fn parse_source(source: &str) -> Result<TranslationUnit, CompilerError> {
        // Tokenize
        let mut lexer = Lexer::new(source);
        let tokens = lexer.tokenize()?;
        
        // Parse
        let mut parser = Parser::new(tokens);
        let ast = parser.parse_translation_unit()?;
        
        Ok(ast)
    }
    
    /// Parse and analyze C99 source code
    pub fn analyze_source(source: &str) -> Result<TranslationUnit, CompilerError> {
        // Parse first
        let mut ast = Self::parse_source(source)?;
        
        // Perform semantic analysis
        let mut analyzer = SemanticAnalyzer::new();
        analyzer.analyze(&mut ast)?;
        
        Ok(ast)
    }
    
    /// Compile C99 source code to IR using typed AST (with GEP for pointer arithmetic)
    pub fn compile_to_ir(source: &str, module_name: &str) -> Result<Module, CompilerError> {
        // Parse first
        let mut ast = Self::parse_source(source)?;
        
        // Perform semantic analysis and extract type information
        let mut analyzer = SemanticAnalyzer::new();
        analyzer.analyze(&mut ast)?;
        let type_analyzer = analyzer.into_type_info();
        
        // Convert to typed AST with symbol type information
        let typed_ast = type_translation_unit(&ast, Rc::clone(&type_analyzer))
            .map_err(|e| CompilerError::semantic_error(e.to_string(), rcc_common::SourceLocation::new_simple(0, 0)))?;
        
        // Generate IR from typed AST
        let codegen = TypedCodeGenerator::new(module_name.to_string());
        let module = codegen.generate(&typed_ast)?;
        
        Ok(module)
    }
    
    /// Tokenize source code (for debugging/IDE features)
    pub fn tokenize_source(source: &str) -> Result<Vec<Token>, CompilerError> {
        let mut lexer = Lexer::new(source);
        lexer.tokenize()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_frontend_parse_simple_function() {
        let source = r#"
int main() {
    return 42;
}
"#;
        
        let ast = Frontend::parse_source(source).unwrap();
        assert_eq!(ast.items.len(), 1);
        
        match &ast.items[0] {
            TopLevelItem::Function(func) => {
                assert_eq!(func.name, "main");
                assert_eq!(func.return_type, Type::Int);
            }
            _ => panic!("Expected function definition"),
        }
    }

    #[test]
    fn test_frontend_tokenize() {
        let source = "int x = 42;";
        let tokens = Frontend::tokenize_source(source).unwrap();
        
        // Should have: int, x, =, 42, ;, EOF
        assert_eq!(tokens.len(), 6);
        assert!(matches!(tokens[0].token_type, TokenType::Int));
        assert!(matches!(tokens[1].token_type, TokenType::Identifier(_)));
        assert!(matches!(tokens[2].token_type, TokenType::Equal));
        assert!(matches!(tokens[3].token_type, TokenType::IntLiteral { value: 42, .. }));
        assert!(matches!(tokens[4].token_type, TokenType::Semicolon));
        assert!(matches!(tokens[5].token_type, TokenType::EndOfFile));
    }

    #[test]
    fn test_frontend_parse_with_variables() {
        let source = r#"
int add(int a, int b) {
    int result = a + b;
    return result;
}
"#;
        
        let ast = Frontend::parse_source(source).unwrap();
        assert_eq!(ast.items.len(), 1);
        
        match &ast.items[0] {
            TopLevelItem::Function(func) => {
                assert_eq!(func.name, "add");
                // Function body should have declaration and return
                match &func.body.kind {
                    StatementKind::Compound(statements) => {
                        assert_eq!(statements.len(), 2);
                    }
                    _ => panic!("Expected compound statement"),
                }
            }
            _ => panic!("Expected function definition"),
        }
    }
}