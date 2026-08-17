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
            
            let suffix = self.parse_integer_suffix()?;
            return Ok(TokenType::IntLiteral { value, suffix, hex: true });
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
        
        let suffix = self.parse_integer_suffix()?;
        Ok(TokenType::IntLiteral { value, suffix, hex: false })
    }

    /// Parse a C99 integer suffix: u/U, l/L, and combinations (ul, lu, …).
    /// `ll`/`ull` are not supported (no `long long` on this target).
    fn parse_integer_suffix(&mut self) -> Result<super::token::IntegerSuffix, CompilerError> {
        use super::token::IntegerSuffix;
        let mut seen_u = false;
        let mut seen_l = false;
        loop {
            match self.current_char() {
                Some('u') | Some('U') if !seen_u => {
                    seen_u = true;
                    self.advance();
                }
                Some('l') | Some('L') if !seen_l => {
                    let first = self.current_char().unwrap();
                    self.advance();
                    if let Some(second) = self.current_char() {
                        if second == first {
                            return Err(CompilerError::lexer_error(
                                "long long integer suffixes (ll/LL) are not supported".to_string(),
                                self.current_location(),
                            ));
                        }
                    }
                    seen_l = true;
                }
                Some('u') | Some('U') | Some('l') | Some('L') => {
                    return Err(CompilerError::lexer_error(
                        "invalid integer suffix".to_string(),
                        self.current_location(),
                    ));
                }
                _ => break,
            }
        }
        Ok(match (seen_u, seen_l) {
            (false, false) => IntegerSuffix::None,
            (true, false) => IntegerSuffix::Unsigned,
            (false, true) => IntegerSuffix::Long,
            (true, true) => IntegerSuffix::UnsignedLong,
        })
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