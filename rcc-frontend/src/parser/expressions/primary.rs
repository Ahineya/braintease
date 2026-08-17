//! Primary expression parsing

use crate::ast::*;
use crate::lexer::{Token, TokenType};
use crate::parser::errors::ParseError;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};

impl Parser {
    /// Parse primary expression
    pub fn parse_primary_expression(&mut self) -> Result<Expression, CompilerError> {
        let start_location = self.current_location();
        
        let kind = match self.advance() {
            Some(Token { token_type: TokenType::IntLiteral { value, suffix, hex }, .. }) => {
                ExpressionKind::IntLiteral { value, suffix, hex }
            }
            Some(Token { token_type: TokenType::FloatLiteral { bits, suffix }, .. }) => {
                ExpressionKind::FloatLiteral { bits, suffix }
            }
            Some(Token { token_type: TokenType::CharLiteral(value), .. }) => {
                ExpressionKind::CharLiteral(value)
            }
            Some(Token { token_type: TokenType::StringLiteral(mut value), .. }) => {
                // Handle adjacent string literal concatenation (C standard feature)
                // "hello" "world" becomes "helloworld"
                while let Some(Token { token_type: TokenType::StringLiteral(next), .. }) = self.peek() {
                    value.push_str(next);
                    self.advance();
                }
                ExpressionKind::StringLiteral(value)
            }
            Some(Token { token_type: TokenType::Identifier(name), .. }) => {
                ExpressionKind::Identifier { name, symbol_id: None }
            }
            Some(Token { token_type: TokenType::LeftParen, .. }) => {
                // Could be cast expression, compound literal, or parenthesized expression
                // Look ahead to determine if this is a cast or compound literal
                if self.is_type_start() {
                    // Try to parse as cast expression or compound literal
                    // We need to be careful here - could still be parenthesized expression with typedef name
                    // Save state for potential backtracking
                    let saved_tokens = self.tokens.clone();
                    
                    // Try to parse type name
                    match self.parse_type_name() {
                        Ok(target_type) => {
                            // Check if we have a closing paren
                            if self.peek().map(|t| &t.token_type) == Some(&TokenType::RightParen) {
                                self.advance(); // consume ')'
                                
                                // Check if this is a compound literal (next token is '{')
                                if self.peek().map(|t| &t.token_type) == Some(&TokenType::LeftBrace) {
                                    // Parse compound literal
                                    let initializer = self.parse_initializer()?;
                                    let end_location = self.current_location();
                                    
                                    return Ok(Expression {
                                        node_id: self.node_id_gen.next(),
                                        kind: ExpressionKind::CompoundLiteral {
                                            type_name: target_type,
                                            initializer: Box::new(initializer),
                                        },
                                        span: SourceSpan::new(start_location, end_location),
                                        expr_type: None,
                                    });
                                } else {
                                    // Cast expression - need a unary expression after the cast
                                    let operand = self.parse_unary_expression()?;
                                    let end_location = operand.span.end.clone();
                                    
                                    return Ok(Expression {
                                        node_id: self.node_id_gen.next(),
                                        kind: ExpressionKind::Cast {
                                            target_type,
                                            operand: Box::new(operand),
                                        },
                                        span: SourceSpan::new(start_location, end_location),
                                        expr_type: None,
                                    });
                                }
                            } else {
                                // Not a cast or compound literal, restore and parse as parenthesized expression
                                self.tokens = saved_tokens;
                            }
                        }
                        Err(_) => {
                            // Failed to parse as type, restore position
                            self.tokens = saved_tokens;
                        }
                    }
                }
                
                // Parse as parenthesized expression
                let expr = self.parse_expression()?;
                self.expect(TokenType::RightParen, "parenthesized expression")?;
                return Ok(expr);
            }
            Some(token) => {
                return Err(ParseError::UnexpectedToken {
                    expected: "primary expression".to_string(),
                    found: token,
                }.into());
            }
            None => {
                return Err(ParseError::UnexpectedEndOfFile {
                    expected: "primary expression".to_string(),
                    location: start_location,
                }.into());
            }
        };
        
        let end_location = self.current_location();
        
        Ok(Expression {
            node_id: self.node_id_gen.next(),
            kind,
            span: SourceSpan::new(start_location, end_location),
            expr_type: None,
        })
    }
}