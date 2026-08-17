//! C99 Recursive Descent Parser
//! 
//! Parses C99 tokens into an Abstract Syntax Tree (AST).
//! Implements a recursive descent parser for the C99 grammar.

pub mod errors;
pub mod types;
pub mod declarations;
pub mod statements;
pub mod expressions;

use crate::ast::*;
use crate::lexer::{Token, TokenType};
use rcc_common::{CompilerError, SourceLocation, SourceSpan};
use std::collections::VecDeque;

pub use errors::ParseError;
use crate::Type;

/// C99 Parser
pub struct Parser {
    pub(crate) tokens: VecDeque<Token>,
    pub(crate) node_id_gen: NodeIdGenerator,
    pub(crate) last_function_params: Option<Vec<(Option<String>, Type, SourceSpan)>>, // Temporary storage for function parameters
    pub(crate) typedef_names: std::collections::HashSet<String>, // Track typedef names for parsing
}

impl Parser {
    /// Create a new parser
    pub fn new(tokens: Vec<Token>) -> Self {
        // Filter out comments and newlines for parsing (keep them for IDE features later)
        let filtered_tokens: Vec<Token> = tokens
            .into_iter()
            .filter(|t| !matches!(t.token_type, 
                TokenType::LineComment(_) | 
                TokenType::BlockComment(_) | 
                TokenType::Newline
            ))
            .collect();
            
        Self {
            tokens: filtered_tokens.into(),
            node_id_gen: NodeIdGenerator::new(),
            last_function_params: None,
            typedef_names: std::collections::HashSet::new(),
        }
    }
    
    /// Peek at current token without consuming
    pub(crate) fn peek(&self) -> Option<&Token> {
        self.tokens.front()
    }
    
    /// Get current token and advance
    pub(crate) fn advance(&mut self) -> Option<Token> {
        self.tokens.pop_front()
    }
    
    /// Check if current token matches expected type
    pub(crate) fn check(&self, token_type: &TokenType) -> bool {
        if let Some(token) = self.peek() {
            std::mem::discriminant(&token.token_type) == std::mem::discriminant(token_type)
        } else {
            matches!(token_type, TokenType::EndOfFile)
        }
    }
    
    /// Check if current token is an identifier
    #[allow(dead_code)]
    pub(crate) fn check_identifier(&self) -> bool {
        matches!(self.peek().map(|t| &t.token_type), Some(TokenType::Identifier(_)))
    }
    
    /// Consume token if it matches expected type
    pub(crate) fn match_token(&mut self, token_type: &TokenType) -> bool {
        if self.check(token_type) {
            self.advance();
            true
        } else {
            false
        }
    }
    
    /// Expect and consume a specific token type
    pub(crate) fn expect(&mut self, token_type: TokenType, context: &str) -> Result<Token, ParseError> {
        if let Some(token) = self.advance() {
            if std::mem::discriminant(&token.token_type) == std::mem::discriminant(&token_type) {
                Ok(token)
            } else {
                Err(ParseError::UnexpectedToken {
                    expected: format!("{token_type} in {context}"),
                    found: token,
                })
            }
        } else {
            let location = SourceLocation::new_simple(0, 0); // TODO: Better EOF location tracking
            Err(ParseError::UnexpectedEndOfFile {
                expected: format!("{token_type} in {context}"),
                location,
            })
        }
    }
    
    /// Get current location for error reporting
    pub(crate) fn current_location(&self) -> SourceLocation {
        if let Some(token) = self.peek() {
            token.span.start.clone()
        } else {
            SourceLocation::new_simple(0, 0) // TODO: Track actual EOF location
        }
    }
    
    /// Parse a complete translation unit
    pub fn parse_translation_unit(&mut self) -> Result<TranslationUnit, CompilerError> {
        let start_location = self.current_location();
        let mut items = Vec::new();
        
        // Parse all top-level items until EOF
        while !self.check(&TokenType::EndOfFile) {
            items.push(self.parse_top_level_item()?);
        }
        
        let end_location = self.current_location();
        
        Ok(TranslationUnit {
            node_id: self.node_id_gen.next(),
            items,
            span: SourceSpan::new(start_location, end_location),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lexer::Lexer;

    fn parse_expression_from_str(input: &str) -> Result<Expression, CompilerError> {
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize()?;
        let mut parser = Parser::new(tokens);
        parser.parse_expression()
    }
    
    fn parse_statement_from_str(input: &str) -> Result<Statement, CompilerError> {
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize()?;
        let mut parser = Parser::new(tokens);
        parser.parse_statement()
    }

    #[test]
    fn test_parse_integer_literal() {
        let expr = parse_expression_from_str("42").unwrap();
        match expr.kind {
            ExpressionKind::IntLiteral(value) => assert_eq!(value, 42),
            _ => panic!("Expected integer literal"),
        }
    }

    #[test]
    fn test_parse_identifier() {
        let expr = parse_expression_from_str("variable").unwrap();
        match expr.kind {
            ExpressionKind::Identifier { name, .. } => assert_eq!(name, "variable"),
            _ => panic!("Expected identifier"),
        }
    }

    #[test]
    fn test_parse_binary_expression() {
        let expr = parse_expression_from_str("2 + 3").unwrap();
        match expr.kind {
            ExpressionKind::Binary { op, left, right } => {
                assert_eq!(op, BinaryOp::Add);
                match (&left.kind, &right.kind) {
                    (ExpressionKind::IntLiteral(2), ExpressionKind::IntLiteral(3)) => {},
                    _ => panic!("Expected 2 + 3"),
                }
            }
            _ => panic!("Expected binary expression"),
        }
    }

    #[test]
    fn test_parse_function_call() {
        let expr = parse_expression_from_str("foo(1, 2)").unwrap();
        match expr.kind {
            ExpressionKind::Call { function, arguments } => {
                match &function.kind {
                    ExpressionKind::Identifier { name, .. } => assert_eq!(name, "foo"),
                    _ => panic!("Expected function identifier"),
                }
                assert_eq!(arguments.len(), 2);
            }
            _ => panic!("Expected function call"),
        }
    }

    #[test]
    fn test_parse_unary_expression() {
        let expr = parse_expression_from_str("-42").unwrap();
        match expr.kind {
            ExpressionKind::Unary { op, operand } => {
                assert_eq!(op, UnaryOp::Minus);
                match &operand.kind {
                    ExpressionKind::IntLiteral(42) => {},
                    _ => panic!("Expected -42"),
                }
            }
            _ => panic!("Expected unary expression"),
        }
    }

    #[test]
    fn test_parse_if_statement() {
        let stmt = parse_statement_from_str("if (x > 0) return 1;").unwrap();
        match stmt.kind {
            StatementKind::If { condition, then_stmt, else_stmt } => {
                match condition.kind {
                    ExpressionKind::Binary { op: BinaryOp::Greater, .. } => {},
                    _ => panic!("Expected comparison condition"),
                }
                match &then_stmt.kind {
                    StatementKind::Return(Some(_)) => {},
                    _ => panic!("Expected return statement"),
                }
                assert!(else_stmt.is_none());
            }
            _ => panic!("Expected if statement"),
        }
    }

    #[test]
    fn test_parse_compound_statement() {
        let stmt = parse_statement_from_str("{ int x = 5; return x; }").unwrap();
        match stmt.kind {
            StatementKind::Compound(statements) => {
                assert_eq!(statements.len(), 2);
                match &statements[0].kind {
                    StatementKind::Declaration { .. } => {},
                    _ => panic!("Expected declaration"),
                }
                match &statements[1].kind {
                    StatementKind::Return(_) => {},
                    _ => panic!("Expected return statement"),
                }
            }
            _ => panic!("Expected compound statement"),
        }
    }

    #[test]
    fn test_parse_simple_function() {
        let input = "int main() { return 42; }";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let mut parser = Parser::new(tokens);
        
        let translation_unit = parser.parse_translation_unit().unwrap();
        assert_eq!(translation_unit.items.len(), 1);
        
        match &translation_unit.items[0] {
            TopLevelItem::Function(func) => {
                assert_eq!(func.name, "main");
                assert_eq!(func.return_type, Type::Int);
                match &func.body.kind {
                    StatementKind::Compound(statements) => {
                        assert_eq!(statements.len(), 1);
                        match &statements[0].kind {
                            StatementKind::Return(Some(expr)) => {
                                match &expr.kind {
                                    ExpressionKind::IntLiteral(42) => {},
                                    _ => panic!("Expected return 42"),
                                }
                            }
                            _ => panic!("Expected return statement"),
                        }
                    }
                    _ => panic!("Expected compound statement"),
                }
            }
            _ => panic!("Expected function definition"),
        }
    }

    #[test]
    fn test_operator_precedence() {
        let expr = parse_expression_from_str("2 + 3 * 4").unwrap();
        
        // Should parse as 2 + (3 * 4)
        match expr.kind {
            ExpressionKind::Binary { op: BinaryOp::Add, left, right } => {
                match (&left.kind, &right.kind) {
                    (ExpressionKind::IntLiteral(2), ExpressionKind::Binary { op: BinaryOp::Mul, .. }) => {},
                    _ => panic!("Expected 2 + (3 * 4) structure"),
                }
            }
            _ => panic!("Expected binary addition"),
        }
    }

    #[test]
    fn test_parse_switch_statement() {
        let stmt = parse_statement_from_str("switch (x) { case 1: return 1; default: return 0; }").unwrap();
        match stmt.kind {
            StatementKind::Switch { body, .. } => {
                match &body.kind {
                    StatementKind::Compound(statements) => {
                        assert_eq!(statements.len(), 2);
                        assert!(matches!(statements[0].kind, StatementKind::Case { .. }));
                        assert!(matches!(statements[1].kind, StatementKind::Default { .. }));
                    }
                    _ => panic!("Expected compound switch body"),
                }
            }
            _ => panic!("Expected switch statement"),
        }
    }

    #[test]
    fn test_parse_variadic_function() {
        let input = "int sum(int n, ...) { return n; }";
        let mut lexer = Lexer::new(input);
        let tokens = lexer.tokenize().unwrap();
        let mut parser = Parser::new(tokens);
        let translation_unit = parser.parse_translation_unit().unwrap();
        match &translation_unit.items[0] {
            TopLevelItem::Function(func) => {
                assert_eq!(func.name, "sum");
                assert!(func.is_variadic);
                assert_eq!(func.parameters.len(), 1);
            }
            _ => panic!("Expected function definition"),
        }
    }
}