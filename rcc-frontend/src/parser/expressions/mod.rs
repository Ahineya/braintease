//! Expression parsing for C99
//! 
//! This module handles parsing of all expression types using operator precedence parsing.

mod primary;
mod postfix;
mod unary;
mod binary;
mod assignment;

use crate::ast::*;
use crate::lexer::TokenType;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};

impl Parser {
    /// Parse expression (top level). Comma has the lowest precedence.
    pub fn parse_expression(&mut self) -> Result<Expression, CompilerError> {
        let mut left = self.parse_assignment_expression()?;

        while self.match_token(&TokenType::Comma) {
            let right = self.parse_assignment_expression()?;
            let span = SourceSpan::new(left.span.start.clone(), right.span.end.clone());
            left = Expression {
                node_id: self.node_id_gen.next(),
                kind: ExpressionKind::Binary {
                    op: BinaryOp::Comma,
                    left: Box::new(left),
                    right: Box::new(right),
                },
                span,
                expr_type: None,
            };
        }

        Ok(left)
    }
}