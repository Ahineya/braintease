//! Unary expression parsing

use crate::ast::*;
use crate::lexer::TokenType;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};

impl Parser {
    /// Parse unary expression
    pub fn parse_unary_expression(&mut self) -> Result<Expression, CompilerError> {
        // Special handling for sizeof
        if self.peek().map(|t| &t.token_type) == Some(&TokenType::Sizeof) {
            let start = self.current_location();
            self.advance(); // consume 'sizeof'
            
            // Check if next token is '(' to determine if it's sizeof(type) or sizeof expr
            if self.peek().map(|t| &t.token_type) == Some(&TokenType::LeftParen) {
                let saved_tokens = self.tokens.clone();
                self.advance(); // consume '('

                if self.is_type_start() {
                    if let Ok(type_spec) = self.parse_type_name() {
                        if self.peek().map(|t| &t.token_type) == Some(&TokenType::RightParen) {
                            self.advance(); // consume ')'
                            let end = self.current_location();
                            let resolved = self.resolve_known_typedefs(&type_spec);

                            return Ok(Expression {
                                node_id: self.node_id_gen.next(),
                                kind: ExpressionKind::SizeofType(resolved),
                                span: SourceSpan::new(start, end),
                                expr_type: None,
                            });
                        }
                    }
                }

                // Not a type, restore and parse as expression
                self.tokens = saved_tokens;
                self.advance(); // re-consume '('
                let expr = self.parse_expression()?;
                self.expect(TokenType::RightParen, "sizeof expression")?;
                
                let end = self.current_location();
                return Ok(Expression {
                    node_id: self.node_id_gen.next(),
                    kind: ExpressionKind::SizeofExpr(Box::new(expr)),
                    span: SourceSpan::new(start, end),
                    expr_type: None,
                });
            }
            
            // Parse as sizeof expression (without parens)
            let operand = self.parse_unary_expression()?;
            let span = SourceSpan::new(start, operand.span.end.clone());
            
            return Ok(Expression {
                node_id: self.node_id_gen.next(),
                kind: ExpressionKind::SizeofExpr(Box::new(operand)),
                span,
                expr_type: None,
            });
        }
        
        // Normal unary operators
        if let Some(op) = self.parse_unary_operator() {
            let operand = self.parse_unary_expression()?;
            let span = SourceSpan::new(self.current_location(), operand.span.end.clone());
            
            Ok(Expression {
                node_id: self.node_id_gen.next(),
                kind: ExpressionKind::Unary {
                    op,
                    operand: Box::new(operand),
                },
                span,
                expr_type: None,
            })
        } else {
            self.parse_postfix_expression()
        }
    }
    
    /// Parse unary operator
    fn parse_unary_operator(&mut self) -> Option<UnaryOp> {
        match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Plus) => { self.advance(); Some(UnaryOp::Plus) }
            Some(TokenType::Minus) => { self.advance(); Some(UnaryOp::Minus) }
            Some(TokenType::Bang) => { self.advance(); Some(UnaryOp::LogicalNot) }
            Some(TokenType::Tilde) => { self.advance(); Some(UnaryOp::BitNot) }
            Some(TokenType::Star) => { self.advance(); Some(UnaryOp::Dereference) }
            Some(TokenType::Ampersand) => { self.advance(); Some(UnaryOp::AddressOf) }
            Some(TokenType::PlusPlus) => { self.advance(); Some(UnaryOp::PreIncrement) }
            Some(TokenType::MinusMinus) => { self.advance(); Some(UnaryOp::PreDecrement) }
            // Sizeof is handled specially in parse_unary_expression
            _ => None,
        }
    }
}