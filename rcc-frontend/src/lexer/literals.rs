//! Literal parsing for the C99 lexer
//! 
//! This module handles parsing of integer, character, and string literals.

use crate::lexer::{Lexer, TokenType};
use rcc_common::CompilerError;

impl Lexer {
    /// Tokenize an integer or floating literal starting at a digit.
    pub fn tokenize_number(&mut self) -> Result<TokenType, CompilerError> {
        // Hex is always integer (hex floats are not supported yet).
        if self.current_char() == Some('0')
            && matches!(self.peek_char(1), Some('x') | Some('X'))
        {
            return self.tokenize_integer();
        }

        let mut number = String::new();
        while let Some(ch) = self.current_char() {
            if ch.is_ascii_digit() {
                number.push(ch);
                self.advance();
            } else {
                break;
            }
        }

        let is_float = match self.current_char() {
            Some('.') => self.peek_char(1) != Some('.'),
            Some('e') | Some('E') => true,
            _ => false,
        };
        if is_float {
            return self.tokenize_float(number);
        }

        self.finish_integer(number)
    }

    /// Tokenize an integer literal
    pub fn tokenize_integer(&mut self) -> Result<TokenType, CompilerError> {
        let mut number = String::new();
        
        // Handle hex prefix
        if self.current_char() == Some('0') && matches!(self.peek_char(1), Some('x') | Some('X')) {
            number.push('0');
            number.push('x');
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
            
            let value = u64::from_str_radix(&number[2..], 16)
                .map_err(|_| CompilerError::lexer_error(
                    format!("Invalid hex literal: {number}"),
                    self.current_location(),
                ))? as i64;
            
            let suffix = self.parse_integer_suffix()?;
            return Ok(TokenType::IntLiteral { value, suffix, hex: true });
        }
        
        // Decimal, or octal if it starts with 0 (C99 6.4.4.1).
        while let Some(ch) = self.current_char() {
            if ch.is_ascii_digit() {
                number.push(ch);
                self.advance();
            } else {
                break;
            }
        }

        self.finish_integer(number)
    }

    fn finish_integer(&mut self, number: String) -> Result<TokenType, CompilerError> {
        let octal = number.starts_with('0') && number.len() > 1;
        if octal && number.chars().any(|c| c == '8' || c == '9') {
            return Err(CompilerError::lexer_error(
                format!("Invalid octal literal: {number}"),
                self.current_location(),
            ));
        }

        let value = if octal {
            u64::from_str_radix(&number, 8).map_err(|_| CompilerError::lexer_error(
                format!("Invalid octal literal: {number}"),
                self.current_location(),
            ))? as i64
        } else {
            number.parse::<u64>().map_err(|_| CompilerError::lexer_error(
                format!("Invalid integer literal: {number}"),
                self.current_location(),
            ))? as i64
        };
        
        let suffix = self.parse_integer_suffix()?;
        // Octal uses the same type ladder as hex (unsigned int before long).
        Ok(TokenType::IntLiteral { value, suffix, hex: octal })
    }

    /// Continue a floating literal after the integer part (which may be empty for `.5`).
    pub fn tokenize_float(&mut self, mut number: String) -> Result<TokenType, CompilerError> {
        if self.current_char() == Some('.') {
            number.push('.');
            self.advance();
            while let Some(ch) = self.current_char() {
                if ch.is_ascii_digit() {
                    number.push(ch);
                    self.advance();
                } else {
                    break;
                }
            }
        }

        if matches!(self.current_char(), Some('e') | Some('E')) {
            number.push(self.current_char().unwrap());
            self.advance();
            if matches!(self.current_char(), Some('+') | Some('-')) {
                number.push(self.current_char().unwrap());
                self.advance();
            }
            let mut exp_digits = 0u32;
            while let Some(ch) = self.current_char() {
                if ch.is_ascii_digit() {
                    number.push(ch);
                    self.advance();
                    exp_digits += 1;
                } else {
                    break;
                }
            }
            if exp_digits == 0 {
                return Err(CompilerError::lexer_error(
                    format!("Invalid floating literal: {number}"),
                    self.current_location(),
                ));
            }
        }

        let suffix = match self.current_char() {
            Some('f') | Some('F') => {
                self.advance();
                super::token::FloatSuffix::Float
            }
            Some('l') | Some('L') => {
                self.advance();
                super::token::FloatSuffix::LongDouble
            }
            _ => super::token::FloatSuffix::None,
        };

        let bits = if suffix == super::token::FloatSuffix::Float {
            number.parse::<f32>().map(|v| v.to_bits() as u64).map_err(|_| {
                CompilerError::lexer_error(
                    format!("Invalid floating literal: {number}"),
                    self.current_location(),
                )
            })?
        } else {
            number.parse::<f64>().map(|v| v.to_bits()).map_err(|_| {
                CompilerError::lexer_error(
                    format!("Invalid floating literal: {number}"),
                    self.current_location(),
                )
            })?
        };

        Ok(TokenType::FloatLiteral { bits, suffix })
    }

    /// Parse a C99 integer suffix: u/U, l/L, ll/LL, and combinations (ul, lu, ull, llu, …).
    fn parse_integer_suffix(&mut self) -> Result<super::token::IntegerSuffix, CompilerError> {
        use super::token::IntegerSuffix;
        let mut seen_u = false;
        let mut seen_l = false;
        let mut seen_ll = false;
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
                            self.advance();
                            seen_ll = true;
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
        Ok(match (seen_u, seen_l, seen_ll) {
            (false, false, false) => IntegerSuffix::None,
            (true, false, false) => IntegerSuffix::Unsigned,
            (false, true, false) => IntegerSuffix::Long,
            (true, true, false) => IntegerSuffix::UnsignedLong,
            (false, true, true) => IntegerSuffix::LongLong,
            (true, true, true) => IntegerSuffix::UnsignedLongLong,
            (_, false, true) => unreachable!("ll implies l"),
        })
    }
    
    /// Parse a C99 simple / octal / hex escape after a consumed backslash.
    fn parse_escape_byte(&mut self) -> Result<u8, CompilerError> {
        match self.current_char() {
            Some('n') => { self.advance(); Ok(b'\n') }
            Some('t') => { self.advance(); Ok(b'\t') }
            Some('r') => { self.advance(); Ok(b'\r') }
            Some('a') => { self.advance(); Ok(7) }
            Some('b') => { self.advance(); Ok(8) }
            Some('f') => { self.advance(); Ok(12) }
            Some('v') => { self.advance(); Ok(11) }
            Some('\\') => { self.advance(); Ok(b'\\') }
            Some('\'') => { self.advance(); Ok(b'\'') }
            Some('"') => { self.advance(); Ok(b'"') }
            Some('?') => { self.advance(); Ok(b'?') }
            Some('x') => {
                self.advance();
                let mut value = 0u32;
                let mut digits = 0u32;
                while let Some(ch) = self.current_char() {
                    let digit = match ch {
                        '0'..='9' => ch as u32 - '0' as u32,
                        'a'..='f' => ch as u32 - 'a' as u32 + 10,
                        'A'..='F' => ch as u32 - 'A' as u32 + 10,
                        _ => break,
                    };
                    self.advance();
                    digits += 1;
                    value = value * 16 + digit;
                    if value > 255 {
                        return Err(CompilerError::lexer_error(
                            "Hex escape sequence out of range".to_string(),
                            self.current_location(),
                        ));
                    }
                }
                if digits == 0 {
                    return Err(CompilerError::lexer_error(
                        "Invalid hex escape sequence".to_string(),
                        self.current_location(),
                    ));
                }
                Ok(value as u8)
            }
            Some(ch) if ('0'..='7').contains(&ch) => {
                let mut value = 0u32;
                let mut digits = 0u32;
                while digits < 3 {
                    let Some(ch) = self.current_char() else { break };
                    if !('0'..='7').contains(&ch) {
                        break;
                    }
                    self.advance();
                    digits += 1;
                    value = value * 8 + (ch as u32 - '0' as u32);
                    if value > 255 {
                        return Err(CompilerError::lexer_error(
                            "Octal escape sequence out of range".to_string(),
                            self.current_location(),
                        ));
                    }
                }
                Ok(value as u8)
            }
            Some(c) => Err(CompilerError::lexer_error(
                format!("Invalid escape sequence: \\{c}"),
                self.current_location(),
            )),
            None => Err(CompilerError::lexer_error(
                "Unterminated escape sequence".to_string(),
                self.current_location(),
            )),
        }
    }

    /// Tokenize a character literal
    pub fn tokenize_char_literal(&mut self) -> Result<TokenType, CompilerError> {
        self.advance(); // Skip opening quote
        
        let ch = match self.current_char() {
            Some('\\') => {
                self.advance(); // Skip backslash
                self.parse_escape_byte()?
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
                    let byte = self.parse_escape_byte()?;
                    string.push(byte as char);
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