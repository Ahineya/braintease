//! Type parsing for C99
//! 
//! This module handles parsing of type specifiers, declarators, and type-related constructs.

use log::warn;
use crate::ast::*;
use crate::lexer::{Token, TokenType};
use crate::parser::errors::ParseError;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};
use crate::{EnumVariant, StorageClass, StructField, Type};

/// Declarator suffix (arrays, functions)
enum DeclSuffix {
    Array(Option<u64>),
    Function { parameters: Vec<Type>, is_variadic: bool },
}

impl Parser {
    /// Parse storage class specifier
    pub fn parse_storage_class(&mut self) -> StorageClass {
        match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Auto) => { self.advance(); StorageClass::Auto }
            Some(TokenType::Static) => { self.advance(); StorageClass::Static }
            Some(TokenType::Extern) => { self.advance(); StorageClass::Extern }
            Some(TokenType::Register) => { self.advance(); StorageClass::Register }
            Some(TokenType::Typedef) => { self.advance(); StorageClass::Typedef }
            _ => StorageClass::Auto, // Default storage class
        }
    }
    
    /// Parse type specifier
    pub fn parse_type_specifier(&mut self) -> Result<Type, CompilerError> {
        let location = self.current_location();
        
        // Skip type qualifiers (const, volatile) - we ignore them for now
        while let Some(token) = self.peek() {
            match &token.token_type {
                TokenType::Const | TokenType::Volatile => {
                    warn!("Ignoring type qualifier: {:?}", token.token_type);
                    self.advance();
                }
                _ => break,
            }
        }
        
        match self.peek().map(|t| &t.token_type) {
            Some(TokenType::Void) => { self.advance(); Ok(Type::Void) }
            Some(TokenType::Char) => { self.advance(); Ok(Type::Char) }
            Some(TokenType::Short) => { self.advance(); Ok(Type::Short) }
            Some(TokenType::Int) => { self.advance(); Ok(Type::Int) }
            Some(TokenType::Long) => { self.advance(); Ok(Type::Long) }
            Some(TokenType::Float) => { self.advance(); Ok(Type::Float) }
            Some(TokenType::Double) => { self.advance(); Ok(Type::Double) }
            Some(TokenType::Signed) => {
                self.advance();
                // Handle "signed int", "signed char", etc.
                match self.peek().map(|t| &t.token_type) {
                    Some(TokenType::Char) => { self.advance(); Ok(Type::SignedChar) }
                    Some(TokenType::Int) | None => Ok(Type::Int), // "signed" defaults to int
                    _ => Ok(Type::Int),
                }
            }
            Some(TokenType::Unsigned) => {
                self.advance();
                match self.peek().map(|t| &t.token_type) {
                    Some(TokenType::Char) => { self.advance(); Ok(Type::UnsignedChar) }
                    Some(TokenType::Short) => { self.advance(); Ok(Type::UnsignedShort) }
                    Some(TokenType::Int) => { self.advance(); Ok(Type::UnsignedInt) }
                    Some(TokenType::Long) => { self.advance(); Ok(Type::UnsignedLong) }
                    _ => Ok(Type::UnsignedInt), // "unsigned" defaults to unsigned int
                }
            }
            Some(TokenType::Struct) => {
                self.advance();
                self.parse_struct_type()
            }
            Some(TokenType::Union) => {
                self.advance();
                self.parse_union_type()
            }
            Some(TokenType::Enum) => {
                self.advance();
                self.parse_enum_type()
            }
            Some(TokenType::Identifier(name)) if self.typedef_names.contains(name) => {
                // This is a typedef name being used as a type specifier
                let name = name.clone();
                self.advance();
                Ok(Type::Typedef(name))
            }
            _ => {
                Err(ParseError::InvalidType {
                    message: "Expected type specifier".to_string(),
                    location,
                }.into())
            }
        }
    }
    
    /// Parse struct type (simplified for MVP - no bitfields)
    pub fn parse_struct_type(&mut self) -> Result<Type, CompilerError> {
        let name = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.peek() {
            let name = name.clone();
            self.advance();
            Some(name)
        } else {
            None
        };
        
        let fields = if self.match_token(&TokenType::LeftBrace) {
            let mut fields = Vec::new();
            
            while !self.check(&TokenType::RightBrace) && !self.check(&TokenType::EndOfFile) {
                let field_type = self.parse_type_specifier()?;
                let (field_name, full_field_type) = self.parse_declarator(field_type)?;
                
                fields.push(StructField {
                    name: field_name,
                    field_type: full_field_type,
                    offset: None, // Computed during semantic analysis
                });
                
                self.expect(TokenType::Semicolon, "struct field")?;
            }
            
            self.expect(TokenType::RightBrace, "struct definition")?;
            fields
        } else {
            Vec::new() // Forward declaration
        };
        
        Ok(Type::Struct { name, fields })
    }
    
    /// Parse union type
    pub fn parse_union_type(&mut self) -> Result<Type, CompilerError> {
        // Similar to struct parsing
        let name = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.peek() {
            let name = name.clone();
            self.advance();
            Some(name)
        } else {
            None
        };
        
        let fields = if self.match_token(&TokenType::LeftBrace) {
            let mut fields = Vec::new();
            
            while !self.check(&TokenType::RightBrace) && !self.check(&TokenType::EndOfFile) {
                let field_type = self.parse_type_specifier()?;
                let (field_name, full_field_type) = self.parse_declarator(field_type)?;
                
                fields.push(StructField {
                    name: field_name,
                    field_type: full_field_type,
                    offset: None,
                });
                
                self.expect(TokenType::Semicolon, "union field")?;
            }
            
            self.expect(TokenType::RightBrace, "union definition")?;
            fields
        } else {
            Vec::new()
        };
        
        Ok(Type::Union { name, fields })
    }
    
    /// Parse enum type
    pub fn parse_enum_type(&mut self) -> Result<Type, CompilerError> {
        let name = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.peek() {
            let name = name.clone();
            self.advance();
            Some(name)
        } else {
            None
        };
        
        let variants = if self.match_token(&TokenType::LeftBrace) {
            let mut variants = Vec::new();
            let mut current_value = 0i64;
            
            while !self.check(&TokenType::RightBrace) && !self.check(&TokenType::EndOfFile) {
                let variant_name = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.advance() {
                    name
                } else {
                    return Err(ParseError::InvalidType {
                        message: "Expected enum variant name".to_string(),
                        location: self.current_location(),
                    }.into());
                };
                
                let value = if self.match_token(&TokenType::Equal) {
                    // Parse constant expression for enum value
                    let expr = self.parse_primary_expression()?;
                    if let ExpressionKind::IntLiteral(val) = expr.kind {
                        current_value = val;
                        val
                    } else {
                        return Err(ParseError::InvalidExpression {
                            message: "Enum values must be integer constants".to_string(),
                            location: expr.span.start,
                        }.into());
                    }
                } else {
                    current_value
                };
                
                variants.push(EnumVariant {
                    name: variant_name,
                    value,
                });
                
                current_value += 1;
                
                if !self.match_token(&TokenType::Comma) {
                    break;
                }
            }
            
            self.expect(TokenType::RightBrace, "enum definition")?;
            variants
        } else {
            Vec::new()
        };
        
        Ok(Type::Enum { name, variants })
    }
    
    /// Parse declarator (handles pointers, arrays, function parameters)
    pub fn parse_declarator(&mut self, base_type: Type) -> Result<(String, Type), CompilerError> {
        // Parse pointer prefix
        let mut current_type = base_type;
        while self.match_token(&TokenType::Star) {
            current_type = Type::Pointer { target: Box::new(current_type), bank: None };
        }
        
        // Parse direct declarator
        self.parse_direct_declarator(current_type)
    }
    
    /// Parse direct declarator
    pub fn parse_direct_declarator(&mut self, base_type: Type) -> Result<(String, Type), CompilerError> {
        // Check for parenthesized declarator first
        if self.check(&TokenType::LeftParen) {
            // Look ahead to see if this is a parenthesized declarator
            let is_parenthesized = if let Some(next_token) = self.tokens.get(1) {
                // If we see a star after '(', it's definitely a parenthesized declarator
                matches!(next_token.token_type, TokenType::Star)
            } else {
                false
            };
            
            if is_parenthesized {
                self.advance(); // consume '('
                
                // Parse the inner declarator (e.g., *parr)
                let (name, inner_ptr_type) = self.parse_declarator(Type::Void)?;
                
                self.expect(TokenType::RightParen, "parenthesized declarator")?;
                
                // Now parse any array/function suffixes that apply after the parentheses
                // For `int (*parr)[4]`, after parsing (*parr), we need to parse [4]
                // Collect suffixes first
                let mut suffixes = Vec::new();
                
                // Apply suffixes (arrays, functions)
                loop {
                    if self.match_token(&TokenType::LeftBracket) {
                        // Array suffix - this array contains elements of the current type
                        let size = if self.check(&TokenType::RightBracket) {
                            None
                        } else {
                            let size_expr = self.parse_assignment_expression()?;
                            if let ExpressionKind::IntLiteral(size) = size_expr.kind {
                                if size >= 0 {
                                    Some(size as u64)
                                } else {
                                    return Err(ParseError::InvalidExpression {
                                        message: "Array size must be non-negative".to_string(),
                                        location: size_expr.span.start,
                                    }.into());
                                }
                            } else {
                                return Err(ParseError::InvalidExpression {
                                    message: "Array size must be a constant expression".to_string(),
                                    location: size_expr.span.start,
                                }.into());
                            }
                        };
                        
                        self.expect(TokenType::RightBracket, "array declarator")?;
                        
                        suffixes.push(DeclSuffix::Array(size));
                    } else if self.match_token(&TokenType::LeftParen) {
                        // Function suffix
                        let mut parameter_types = Vec::new();
                        let mut parameter_info = Vec::new();
                        
                        if !self.check(&TokenType::RightParen) {
                            loop {
                                if parameter_types.is_empty() && self.check(&TokenType::Void) {
                                    if let Some(next_token) = self.tokens.get(1) {
                                        if matches!(next_token.token_type, TokenType::RightParen) {
                                            self.advance(); // consume void
                                            break;
                                        }
                                    }
                                }
                                
                                let param_start = self.current_location();
                                let param_type = self.parse_type_specifier()?;
                                
                                let (param_name, full_param_type) = if matches!(self.peek().map(|t| &t.token_type), 
                                    Some(TokenType::Star) | Some(TokenType::Identifier(_))) {
                                    let (name, full_type) = self.parse_declarator(param_type)?;
                                    (Some(name), full_type)
                                } else {
                                    (None, param_type)
                                };
                                
                                let param_end = self.current_location();
                                
                                parameter_types.push(full_param_type.clone());
                                parameter_info.push((param_name, full_param_type, SourceSpan::new(param_start, param_end)));
                                
                                if !self.match_token(&TokenType::Comma) {
                                    break;
                                }
                            }
                        }
                        
                        self.expect(TokenType::RightParen, "function declarator")?;
                        self.last_function_params = Some(parameter_info);
                        
                        suffixes.push(DeclSuffix::Function {
                            parameters: parameter_types,
                            is_variadic: false,
                        });
                    } else {
                        break;
                    }
                }
                
                // Apply suffixes in reverse order
                let mut final_type = base_type;
                for suffix in suffixes.into_iter().rev() {
                    match suffix {
                        DeclSuffix::Array(size) => {
                            final_type = Type::Array {
                                element_type: Box::new(final_type),
                                size,
                            };
                        }
                        DeclSuffix::Function { parameters, is_variadic } => {
                            final_type = Type::Function {
                                return_type: Box::new(final_type),
                                parameters,
                                is_variadic,
                            };
                        }
                    }
                }
                
                // Now combine the pointer type from the inner declarator with the final type
                // For `int (*parr)[4]`:
                // - inner_ptr_type should be Pointer (from *parr)
                // - final_type should be Array[4] of int
                // - Result: Pointer to Array[4] of int
                
                if matches!(inner_ptr_type, Type::Pointer { .. }) {
                    // Apply the pointer to the final type
                    let result_type = Type::Pointer {
                        target: Box::new(final_type),
                        bank: None,
                    };
                    log::debug!("Parenthesized declarator: name={}, result_type={:?}", name, result_type);
                    return Ok((name, result_type));
                } else {
                    return Ok((name, final_type));
                }
            }
        }
        
        // Not a parenthesized declarator - parse normally
        let name = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.advance() {
            name
        } else {
            return Err(ParseError::InvalidType {
                message: "Expected identifier in declarator".to_string(),
                location: self.current_location(),
            }.into());
        };
        
        // Parse suffix (arrays, function parameters)
        // First collect all array dimensions and function parameters
        let mut suffixes = Vec::new();
        
        loop {
            if self.match_token(&TokenType::LeftBracket) {
                // Array declarator
                let size = if self.check(&TokenType::RightBracket) {
                    None // Incomplete array type
                } else {
                    let size_expr = self.parse_assignment_expression()?;
                    if let ExpressionKind::IntLiteral(size) = size_expr.kind {
                        if size >= 0 {
                            Some(size as u64)
                        } else {
                            return Err(ParseError::InvalidExpression {
                                message: "Array size must be non-negative".to_string(),
                                location: size_expr.span.start,
                            }.into());
                        }
                    } else {
                        return Err(ParseError::InvalidExpression {
                            message: "Array size must be a constant expression".to_string(),
                            location: size_expr.span.start,
                        }.into());
                    }
                };
                
                self.expect(TokenType::RightBracket, "array declarator")?;
                
                suffixes.push(DeclSuffix::Array(size));
                
            } else if self.match_token(&TokenType::LeftParen) {
                // Function declarator
                let mut parameter_types = Vec::new();
                let mut parameter_info = Vec::new(); // Store both names and types
                
                if !self.check(&TokenType::RightParen) {
                    loop {
                        // Check for void parameter list (func(void) means no parameters)
                        // But void* is a valid parameter type, so we need to look ahead
                        if parameter_types.is_empty() && self.check(&TokenType::Void) {
                            // Peek at the next token after void
                            if let Some(next_token) = self.tokens.get(1) {
                                // Only treat bare "void)" as empty parameter list
                                if matches!(next_token.token_type, TokenType::RightParen) {
                                    self.advance(); // consume void
                                    break;
                                }
                            }
                        }
                        
                        let param_start = self.current_location();
                        let param_type = self.parse_type_specifier()?;
                        
                        // Parse declarator for the parameter (handles pointers, arrays, etc.)
                        // Check if we have a declarator (could be *, identifier, or just type)
                        let (param_name, full_param_type) = if matches!(self.peek().map(|t| &t.token_type), 
                            Some(TokenType::Star) | Some(TokenType::Identifier(_))) {
                            let (name, full_type) = self.parse_declarator(param_type)?;
                            (Some(name), full_type)
                        } else {
                            // No declarator, just the type (e.g., in function prototypes like void func(int))
                            (None, param_type)
                        };
                        
                        let param_end = self.current_location();
                        
                        parameter_types.push(full_param_type.clone());
                        parameter_info.push((param_name, full_param_type, SourceSpan::new(param_start, param_end)));
                        
                        if !self.match_token(&TokenType::Comma) {
                            break;
                        }
                    }
                }
                
                self.expect(TokenType::RightParen, "function declarator")?;
                
                // Store parameter info in function type (we'll extract it later)
                // For now, we store it in a thread-local for the parser
                self.last_function_params = Some(parameter_info);
                
                suffixes.push(DeclSuffix::Function { 
                    parameters: parameter_types, 
                    is_variadic: false // TODO: Handle variadic functions
                });
                
            } else {
                break;
            }
        }
        
        // Apply suffixes in reverse order (right-to-left)
        // For int arr[2][3], we have suffixes [Array(2), Array(3)]
        // We need to build: Array(2, Array(3, int))
        let mut current_type = base_type;
        for suffix in suffixes.into_iter().rev() {
            match suffix {
                DeclSuffix::Array(size) => {
                    current_type = Type::Array {
                        element_type: Box::new(current_type),
                        size,
                    };
                }
                DeclSuffix::Function { parameters, is_variadic } => {
                    current_type = Type::Function {
                        return_type: Box::new(current_type),
                        parameters,
                        is_variadic,
                    };
                }
            }
        }
        
        Ok((name, current_type))
    }
    
    /// Check if the current position starts a declaration
    pub fn is_declaration_start(&self) -> bool {
        // Check for storage class or type keywords
        if matches!(self.peek().map(|t| &t.token_type), Some(
            TokenType::Auto | TokenType::Static | TokenType::Extern | TokenType::Register |
            TokenType::Typedef |
            TokenType::Void | TokenType::Char | TokenType::Short | TokenType::Int | 
            TokenType::Long | TokenType::Float | TokenType::Double |
            TokenType::Signed | TokenType::Unsigned |
            TokenType::Struct | TokenType::Union | TokenType::Enum
        )) {
            return true;
        }
        
        // Check if it's a typedef name
        if let Some(TokenType::Identifier(name)) = self.peek().map(|t| &t.token_type) {
            return self.typedef_names.contains(name);
        }
        
        false
    }
    
    /// Check if the current position starts a type
    pub fn is_type_start(&self) -> bool {
        // Check for type keywords
        if matches!(self.peek().map(|t| &t.token_type), Some(
            TokenType::Void | TokenType::Char | TokenType::Short | TokenType::Int | 
            TokenType::Long | TokenType::Float | TokenType::Double |
            TokenType::Signed | TokenType::Unsigned |
            TokenType::Struct | TokenType::Union | TokenType::Enum
        )) {
            return true;
        }
        
        // Check if it's a typedef name
        if let Some(TokenType::Identifier(name)) = self.peek().map(|t| &t.token_type) {
            return self.typedef_names.contains(name);
        }
        
        false
    }
    
    /// Parse a type name (used in cast expressions)
    /// This is like parse_type_specifier + abstract declarator (no identifier required)
    pub fn parse_type_name(&mut self) -> Result<Type, CompilerError> {
        let base_type = self.parse_type_specifier()?;
        self.parse_abstract_declarator(base_type)
    }
    
    /// Parse an abstract declarator (pointer/array/function types without identifier)
    pub fn parse_abstract_declarator(&mut self, base_type: Type) -> Result<Type, CompilerError> {
        // Parse pointer prefix
        let mut current_type = base_type;
        while self.match_token(&TokenType::Star) {
            current_type = Type::Pointer { target: Box::new(current_type), bank: None };
        }
        
        // Parse direct abstract declarator (arrays, function types)
        self.parse_direct_abstract_declarator(current_type)
    }
    
    /// Parse direct abstract declarator
    pub fn parse_direct_abstract_declarator(&mut self, base_type: Type) -> Result<Type, CompilerError> {
        // Collect array suffixes left-to-right, then apply right-to-left
        // so `int[][2]` is array-of-unknown of `int[2]`, not `int[2]` of incomplete arrays.
        let mut suffixes = Vec::new();
        
        loop {
            if self.match_token(&TokenType::LeftBracket) {
                let size = if self.check(&TokenType::RightBracket) {
                    None
                } else {
                    let size_expr = self.parse_assignment_expression()?;
                    if let ExpressionKind::IntLiteral(size) = size_expr.kind {
                        if size >= 0 {
                            Some(size as u64)
                        } else {
                            return Err(ParseError::InvalidExpression {
                                message: "Array size must be non-negative".to_string(),
                                location: size_expr.span.start,
                            }.into());
                        }
                    } else {
                        return Err(ParseError::InvalidExpression {
                            message: "Array size must be a constant expression".to_string(),
                            location: size_expr.span.start,
                        }.into());
                    }
                };
                
                self.expect(TokenType::RightBracket, "array declarator")?;
                suffixes.push(DeclSuffix::Array(size));
            } else {
                break;
            }
        }
        
        let mut current_type = base_type;
        for suffix in suffixes.into_iter().rev() {
            match suffix {
                DeclSuffix::Array(size) => {
                    current_type = Type::Array {
                        element_type: Box::new(current_type),
                        size,
                    };
                }
                DeclSuffix::Function { parameters, is_variadic } => {
                    current_type = Type::Function {
                        return_type: Box::new(current_type),
                        parameters,
                        is_variadic,
                    };
                }
            }
        }
        
        Ok(current_type)
    }
}
