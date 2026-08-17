//! Statement parsing for C99
//! 
//! This module handles parsing of all statement types.

use crate::ast::*;
use crate::lexer::TokenType;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};

impl Parser {
    /// Parse compound statement (block)
    pub fn parse_compound_statement(&mut self) -> Result<Statement, CompilerError> {
        let start_location = self.current_location();
        
        self.expect(TokenType::LeftBrace, "compound statement")?;
        
        let mut statements = Vec::new();
        while !self.check(&TokenType::RightBrace) && !self.check(&TokenType::EndOfFile) {
            statements.push(self.parse_statement()?);
        }
        
        self.expect(TokenType::RightBrace, "compound statement")?;
        
        let end_location = self.current_location();
        
        Ok(Statement {
            node_id: self.node_id_gen.next(),
            kind: StatementKind::Compound(statements),
            span: SourceSpan::new(start_location, end_location),
        })
    }
    
    /// Parse statement
    pub fn parse_statement(&mut self) -> Result<Statement, CompilerError> {
        let start_location = self.current_location();
        
        let kind = match self.peek().map(|t| &t.token_type) {
            Some(TokenType::LeftBrace) => {
                return self.parse_compound_statement();
            }
            Some(TokenType::If) => {
                self.advance();
                self.parse_if_statement()?
            }
            Some(TokenType::While) => {
                self.advance();
                self.parse_while_statement()?
            }
            Some(TokenType::For) => {
                self.advance();
                self.parse_for_statement()?
            }
            Some(TokenType::Do) => {
                self.advance();
                self.parse_do_while_statement()?
            }
            Some(TokenType::Switch) => {
                self.advance();
                self.parse_switch_statement()?
            }
            Some(TokenType::Case) => {
                self.advance();
                self.parse_case_statement()?
            }
            Some(TokenType::Default) => {
                self.advance();
                self.parse_default_statement()?
            }
            Some(TokenType::Return) => {
                self.advance();
                self.parse_return_statement()?
            }
            Some(TokenType::Break) => {
                self.advance();
                self.expect(TokenType::Semicolon, "break statement")?;
                StatementKind::Break
            }
            Some(TokenType::Continue) => {
                self.advance();
                self.expect(TokenType::Semicolon, "continue statement")?;
                StatementKind::Continue
            }
            Some(TokenType::Goto) => {
                self.advance();
                self.parse_goto_statement()?
            }
            Some(TokenType::Asm) => {
                self.advance();
                self.parse_inline_asm_statement()?
            }
            Some(TokenType::Semicolon) => {
                self.advance();
                StatementKind::Empty
            }
            // Check for labeled statement, declaration, or expression statement
            _ => {
                if self.is_labeled_statement_start() {
                    self.parse_labeled_statement()?
                } else if self.is_declaration_start() {
                    self.parse_declaration_statement()?
                } else {
                    self.parse_expression_statement()?
                }
            }
        };
        
        let end_location = self.current_location();
        
        Ok(Statement {
            node_id: self.node_id_gen.next(),
            kind,
            span: SourceSpan::new(start_location, end_location),
        })
    }
    
    /// Parse expression statement
    pub fn parse_expression_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let expr = self.parse_expression()?;
        self.expect(TokenType::Semicolon, "expression statement")?;
        Ok(StatementKind::Expression(expr))
    }
    
    /// Parse if statement
    pub fn parse_if_statement(&mut self) -> Result<StatementKind, CompilerError> {
        self.expect(TokenType::LeftParen, "if statement")?;
        let condition = self.parse_expression()?;
        self.expect(TokenType::RightParen, "if statement")?;
        
        let then_stmt = Box::new(self.parse_statement()?);
        
        let else_stmt = if self.match_token(&TokenType::Else) {
            Some(Box::new(self.parse_statement()?))
        } else {
            None
        };
        
        Ok(StatementKind::If { condition, then_stmt, else_stmt })
    }
    
    /// Parse while statement
    pub fn parse_while_statement(&mut self) -> Result<StatementKind, CompilerError> {
        self.expect(TokenType::LeftParen, "while statement")?;
        let condition = self.parse_expression()?;
        self.expect(TokenType::RightParen, "while statement")?;
        
        let body = Box::new(self.parse_statement()?);
        
        Ok(StatementKind::While { condition, body })
    }
    
    /// Parse for statement
    pub fn parse_for_statement(&mut self) -> Result<StatementKind, CompilerError> {
        self.expect(TokenType::LeftParen, "for statement")?;
        
        // Parse init (can be declaration or expression)
        let init = if self.check(&TokenType::Semicolon) {
            None
        } else if self.is_declaration_start() {
            Some(Box::new(Statement {
                node_id: self.node_id_gen.next(),
                kind: self.parse_declaration_statement()?,
                span: SourceSpan::new(self.current_location(), self.current_location()),
            }))
        } else {
            let expr = self.parse_expression()?;
            self.expect(TokenType::Semicolon, "for statement init")?;
            Some(Box::new(Statement {
                node_id: self.node_id_gen.next(),
                kind: StatementKind::Expression(expr),
                span: SourceSpan::new(self.current_location(), self.current_location()),
            }))
        };
        
        if init.is_none() {
            self.expect(TokenType::Semicolon, "for statement init")?;
        }
        
        // Parse condition
        let condition = if self.check(&TokenType::Semicolon) {
            None
        } else {
            Some(self.parse_expression()?)
        };
        self.expect(TokenType::Semicolon, "for statement condition")?;
        
        // Parse update
        let update = if self.check(&TokenType::RightParen) {
            None
        } else {
            Some(self.parse_expression()?)
        };
        self.expect(TokenType::RightParen, "for statement")?;
        
        let body = Box::new(self.parse_statement()?);
        
        Ok(StatementKind::For { init, condition, update, body })
    }
    
    /// Parse switch statement: `switch (expr) statement`
    pub fn parse_switch_statement(&mut self) -> Result<StatementKind, CompilerError> {
        self.expect(TokenType::LeftParen, "switch statement")?;
        let expression = self.parse_expression()?;
        self.expect(TokenType::RightParen, "switch statement")?;
        let body = Box::new(self.parse_statement()?);
        Ok(StatementKind::Switch { expression, body })
    }

    /// Parse case label: `case constant-expression : statement`
    pub fn parse_case_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let value = self.parse_expression()?;
        self.expect(TokenType::Colon, "case label")?;
        let statement = Box::new(self.parse_statement()?);
        Ok(StatementKind::Case { value, statement })
    }

    /// Parse default label: `default : statement`
    pub fn parse_default_statement(&mut self) -> Result<StatementKind, CompilerError> {
        self.expect(TokenType::Colon, "default label")?;
        let statement = Box::new(self.parse_statement()?);
        Ok(StatementKind::Default { statement })
    }

    /// Parse do-while statement
    pub fn parse_do_while_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let body = Box::new(self.parse_statement()?);
        
        self.expect(TokenType::While, "do-while statement")?;
        self.expect(TokenType::LeftParen, "do-while statement")?;
        let condition = self.parse_expression()?;
        self.expect(TokenType::RightParen, "do-while statement")?;
        self.expect(TokenType::Semicolon, "do-while statement")?;
        
        Ok(StatementKind::DoWhile { body, condition })
    }
    
    /// `identifier :` starts a labeled statement (labels are a separate namespace from typedefs).
    fn is_labeled_statement_start(&self) -> bool {
        matches!(self.peek().map(|t| &t.token_type), Some(TokenType::Identifier(_)))
            && matches!(self.tokens.get(1).map(|t| &t.token_type), Some(TokenType::Colon))
    }

    /// Parse goto statement: `goto identifier ;`
    pub fn parse_goto_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let name = match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Identifier(name)) => {
                let name = name.clone();
                self.advance();
                name
            }
            _ => {
                return Err(CompilerError::parse_error(
                    "Expected identifier after goto".to_string(),
                    self.current_location(),
                ));
            }
        };
        self.expect(TokenType::Semicolon, "goto statement")?;
        Ok(StatementKind::Goto(name))
    }

    /// Parse labeled statement: `identifier : statement`
    pub fn parse_labeled_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let name = match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Identifier(name)) => {
                let name = name.clone();
                self.advance();
                name
            }
            _ => {
                return Err(CompilerError::parse_error(
                    "Expected identifier for label".to_string(),
                    self.current_location(),
                ));
            }
        };
        self.expect(TokenType::Colon, "label")?;
        let statement = Box::new(self.parse_statement()?);
        Ok(StatementKind::Label { name, statement })
    }

    /// Parse return statement
    pub fn parse_return_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let value = if self.check(&TokenType::Semicolon) {
            None
        } else {
            Some(self.parse_expression()?)
        };
        
        self.expect(TokenType::Semicolon, "return statement")?;
        Ok(StatementKind::Return(value))
    }
    
    /// Parse inline assembly statement
    pub fn parse_inline_asm_statement(&mut self) -> Result<StatementKind, CompilerError> {
        // Extended syntax: asm("code" : outputs : inputs : clobbers);
        // Basic syntax: asm("code");
        
        
        self.expect(TokenType::LeftParen, "inline assembly")?;
        
        // Parse assembly code string (with adjacent string concatenation)
        let assembly = match self.peek().map(|t| &t.token_type) {
            Some(TokenType::StringLiteral(s)) => {
                let mut code = s.clone();
                self.advance();
                // Handle adjacent string concatenation
                while let Some(TokenType::StringLiteral(next)) = self.peek().map(|t| &t.token_type) {
                    code.push_str(next);
                    self.advance();
                }
                code
            }
            _ => {
                return Err(CompilerError::parse_error(
                    "Expected string literal for inline assembly".to_string(),
                    self.current_location(),
                ));
            }
        };
        
        let mut outputs = Vec::new();
        let mut inputs = Vec::new();
        let mut clobbers = Vec::new();
        
        // Check for extended syntax (colon after assembly string)
        if self.peek().map(|t| &t.token_type) == Some(&TokenType::Colon) {
            self.advance(); // consume first colon
            
            // Parse output operands
            outputs = self.parse_asm_operands()?;
            
            // Check for input operands
            if self.peek().map(|t| &t.token_type) == Some(&TokenType::Colon) {
                self.advance(); // consume second colon
                inputs = self.parse_asm_operands()?;
                
                // Check for clobbers
                if self.peek().map(|t| &t.token_type) == Some(&TokenType::Colon) {
                    self.advance(); // consume third colon
                    clobbers = self.parse_asm_clobbers()?;
                }
            }
        }
        
        self.expect(TokenType::RightParen, "inline assembly")?;
        self.expect(TokenType::Semicolon, "inline assembly")?;
        
        Ok(StatementKind::InlineAsm { 
            assembly,
            outputs,
            inputs,
            clobbers,
        })
    }
    
    /// Parse assembly operands (outputs or inputs)
    fn parse_asm_operands(&mut self) -> Result<Vec<crate::ast::statements::AsmOperand>, CompilerError> {
        use crate::ast::statements::AsmOperand;
        let mut operands = Vec::new();
        
        // Empty operand list is valid
        if self.peek().map(|t| &t.token_type) == Some(&TokenType::Colon) ||
           self.peek().map(|t| &t.token_type) == Some(&TokenType::RightParen) {
            return Ok(operands);
        }
        
        loop {
            // Parse constraint string
            let constraint = match self.peek().map(|t| &t.token_type) {
                Some(TokenType::StringLiteral(s)) => {
                    let c = s.clone();
                    self.advance();
                    c
                }
                _ => {
                    return Err(CompilerError::parse_error(
                        "Expected constraint string for assembly operand".to_string(),
                        self.current_location(),
                    ));
                }
            };
            
            // Expect parenthesized expression
            self.expect(TokenType::LeftParen, "assembly operand expression")?;
            let expr = self.parse_expression()?;
            self.expect(TokenType::RightParen, "assembly operand expression")?;
            
            operands.push(AsmOperand {
                constraint,
                expr,
            });
            
            // Check for more operands
            if self.peek().map(|t| &t.token_type) != Some(&TokenType::Comma) {
                break;
            }
            self.advance(); // consume comma
        }
        
        Ok(operands)
    }
    
    /// Parse clobber list (register names)
    fn parse_asm_clobbers(&mut self) -> Result<Vec<String>, CompilerError> {
        let mut clobbers = Vec::new();
        
        // Empty clobber list is valid
        if self.peek().map(|t| &t.token_type) == Some(&TokenType::RightParen) {
            return Ok(clobbers);
        }
        
        loop {
            // Parse clobber string (register name)
            match self.peek().map(|t| &t.token_type) {
                Some(TokenType::StringLiteral(s)) => {
                    clobbers.push(s.clone());
                    self.advance();
                }
                _ => {
                    return Err(CompilerError::parse_error(
                        "Expected register name string for clobber".to_string(),
                        self.current_location(),
                    ));
                }
            };
            
            // Check for more clobbers
            if self.peek().map(|t| &t.token_type) != Some(&TokenType::Comma) {
                break;
            }
            self.advance(); // consume comma
        }
        
        Ok(clobbers)
    }
}