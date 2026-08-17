//! Literal parsing for the C99 lexer
//! 
//! This module handles parsing of integer, character, and string literals.

use crate::lexer::{Lexer, TokenType};
use rcc_common::CompilerError;

impl Lexer {
    /// Tokenize an integer literal
    pub fn tokenize_integer(&mut self) -> Result<TokenType, CompilerError> {
        let mut number = String::new();
        
        // Handle hex prefix
        if self.current_char() == Some('0') && self.peek_char(1) == Some('x') {
            number.push_str("0x");
            self.advance(); // '0'
            self.advance(); // 'x'
            
            while let Some(ch) = self.current_char() {
                if ch.is_ascii_hexdigit() {
                    number.push(ch);
                    self.advance();
                } else {
                    break;
                }
            }
            
            if number.len() == 2 {
                return Err(CompilerError::lexer_error(
                    "Invalid hex literal".to_string(),
                    self.current_location(),
                ));
            }
            
            let value = i64::from_str_radix(&number[2..], 16)
                .map_err(|_| CompilerError::lexer_error(
                    format!("Invalid hex literal: {number}"),
                    self.current_location(),
                ))?;
            
            self.skip_integer_suffix();
            return Ok(TokenType::IntLiteral(value));
        }
        
        // Handle decimal numbers
        while let Some(ch) = self.current_char() {
            if ch.is_ascii_digit() {
                number.push(ch);
                self.advance();
            } else {
                break;
            }
        }
        
        let value = number.parse::<i64>()
            .map_err(|_| CompilerError::lexer_error(
                format!("Invalid integer literal: {number}"),
                self.current_location(),
            ))?;
        
        self.skip_integer_suffix();
        Ok(TokenType::IntLiteral(value))
    }

    /// Consume a C99 integer suffix: u/U, l/L, ll/LL, and combinations (ul, lu, ull, …).
    fn skip_integer_suffix(&mut self) {
        while let Some(ch) = self.current_char() {
            if matches!(ch, 'u' | 'U' | 'l' | 'L') {
                self.advance();
            } else {
                break;
            }
        }
    }
    
    /// Tokenize a character literal
    pub fn tokenize_char_literal(&mut self) -> Result<TokenType, CompilerError> {
        self.advance(); // Skip opening quote
        
        let ch = match self.current_char() {
            Some('\\') => {
                self.advance(); // Skip backslash
                match self.current_char() {
                    Some('n') => { self.advance(); b'\n' },
                    Some('t') => { self.advance(); b'\t' },
                    Some('r') => { self.advance(); b'\r' },
                    Some('\\') => { self.advance(); b'\\' },
                    Some('\'') => { self.advance(); b'\'' },
                    Some('0') => { self.advance(); 0 },
                    Some(c) => {
                        return Err(CompilerError::lexer_error(
                            format!("Invalid escape sequence: \\{c}"),
                            self.current_location(),
                        ));
                    }
                    None => {
                        return Err(CompilerError::lexer_error(
                            "Unterminated character literal".to_string(),
                            self.current_location(),
                        ));
                    }
                }
            }
            Some(ch) if ch != '\'' => {
                self.advance();
                ch as u8
            }
            _ => {
                return Err(CompilerError::lexer_error(
                    "Empty character literal".to_string(),
                    self.current_location(),
                ));
            }
        };
        
        if self.current_char() != Some('\'') {
            return Err(CompilerError::lexer_error(
                "Unterminated character literal".to_string(),
                self.current_location(),
            ));
        }
        
        self.advance(); // Skip closing quote
        Ok(TokenType::CharLiteral(ch))
    }
    
    /// Tokenize a string literal
    pub fn tokenize_string_literal(&mut self) -> Result<TokenType, CompilerError> {
        self.advance(); // Skip opening quote
        let mut string = String::new();
        
        while let Some(ch) = self.current_char() {
            match ch {
                '"' => {
                    self.advance();
                    return Ok(TokenType::StringLiteral(string));
                }
                '\\' => {
                    self.advance(); // Skip backslash
                    match self.current_char() {
                        Some('n') => { string.push('\n'); self.advance(); },
                        Some('t') => { string.push('\t'); self.advance(); },
                        Some('r') => { string.push('\r'); self.advance(); },
                        Some('\\') => { string.push('\\'); self.advance(); },
                        Some('"') => { string.push('"'); self.advance(); },
                        Some('0') => { string.push('\0'); self.advance(); },
                        Some(c) => {
                            return Err(CompilerError::lexer_error(
                                format!("Invalid escape sequence: \\{c}"),
                                self.current_location(),
                            ));
                        }
                        None => {
                            return Err(CompilerError::lexer_error(
                                "Unterminated string literal".to_string(),
                                self.current_location(),
                            ));
                        }
                    }
                }
                _ => {
                    string.push(ch);
                    self.advance();
                }
            }
        }
        
        Err(CompilerError::lexer_error(
            "Unterminated string literal".to_string(),
            self.current_location(),
        ))
    }
}