//! Assignment expression parsing

use crate::ast::*;
use crate::lexer::TokenType;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};

impl Parser {
    /// Parse assignment expression
    pub fn parse_assignment_expression(&mut self) -> Result<Expression, CompilerError> {
        let left = self.parse_conditional_expression()?;
        
        // Check for assignment operators
        if let Some(op) = self.parse_assignment_operator() {
            let right = self.parse_assignment_expression()?;
            let span = SourceSpan::new(left.span.start.clone(), right.span.end.clone());
            
            return Ok(Expression {
                node_id: self.node_id_gen.next(),
                kind: ExpressionKind::Binary {
                    op,
                    left: Box::new(left),
                    right: Box::new(right),
                },
                span,
                expr_type: None,
            });
        }
        
        Ok(left)
    }
    
    /// Parse assignment operator
    fn parse_assignment_operator(&mut self) -> Option<BinaryOp> {
        match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Equal) => { self.advance(); Some(BinaryOp::Assign) }
            Some(TokenType::PlusEqual) => { self.advance(); Some(BinaryOp::AddAssign) }
            Some(TokenType::MinusEqual) => { self.advance(); Some(BinaryOp::SubAssign) }
            Some(TokenType::StarEqual) => { self.advance(); Some(BinaryOp::MulAssign) }
            Some(TokenType::SlashEqual) => { self.advance(); Some(BinaryOp::DivAssign) }
            Some(TokenType::PercentEqual) => { self.advance(); Some(BinaryOp::ModAssign) }
            Some(TokenType::AmpersandEqual) => { self.advance(); Some(BinaryOp::BitAndAssign) }
            Some(TokenType::PipeEqual) => { self.advance(); Some(BinaryOp::BitOrAssign) }
            Some(TokenType::CaretEqual) => { self.advance(); Some(BinaryOp::BitXorAssign) }
            Some(TokenType::LeftShiftEqual) => { self.advance(); Some(BinaryOp::LeftShiftAssign) }
            Some(TokenType::RightShiftEqual) => { self.advance(); Some(BinaryOp::RightShiftAssign) }
            _ => None,
        }
    }
}