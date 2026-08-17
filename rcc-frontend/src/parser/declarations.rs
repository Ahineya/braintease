//! Declaration and function parsing for C99
//! 
//! This module handles parsing of declarations, function definitions, and initializers.

use crate::ast::*;
use crate::lexer::{Token, TokenType};
use crate::parser::errors::ParseError;
use crate::parser::Parser;
use rcc_common::{CompilerError, SourceSpan};
use crate::{StorageClass, Type};

impl Parser {
    /// Parse a top-level item (function definition or declaration)
    pub fn parse_top_level_item(&mut self) -> Result<TopLevelItem, CompilerError> {
        // For simplicity in MVP, we'll distinguish function definitions from declarations
        // by looking ahead for a function body (opening brace after parameter list)
        
        // Parse declaration specifiers (storage class, type)
        let storage_class = self.parse_storage_class();
        let base_type = self.parse_type_specifier()?;
        
        // Remember the start location for span tracking
        let start_loc = self.current_location();
        
        // Check if this is a standalone struct/union/enum definition
        match &base_type {
            Type::Struct { name: Some(name), fields, .. } if !fields.is_empty() => {
                // This is a struct definition with a name and fields
                if self.check(&TokenType::Semicolon) {
                    let end_loc = self.current_location();
                    self.advance(); // Consume the semicolon
                    return Ok(TopLevelItem::TypeDefinition {
                        name: name.clone(),
                        type_def: base_type,
                        span: SourceSpan::new(start_loc, end_loc),
                    });
                }
            }
            Type::Union { name: Some(name), fields, .. } if !fields.is_empty() => {
                // This is a union definition with a name and fields
                if self.check(&TokenType::Semicolon) {
                    let end_loc = self.current_location();
                    self.advance(); // Consume the semicolon
                    return Ok(TopLevelItem::TypeDefinition {
                        name: name.clone(),
                        type_def: base_type,
                        span: SourceSpan::new(start_loc, end_loc),
                    });
                }
            }
            Type::Enum { name: Some(name), .. } => {
                // This is an enum definition with a name
                if self.check(&TokenType::Semicolon) {
                    let end_loc = self.current_location();
                    self.advance(); // Consume the semicolon
                    return Ok(TopLevelItem::TypeDefinition {
                        name: name.clone(),
                        type_def: base_type,
                        span: SourceSpan::new(start_loc, end_loc),
                    });
                }
            }
            _ => {}
        }
        
        // Parse first declarator (name and type modifications like pointers, arrays, functions)
        let (name, full_type) = self.parse_declarator(base_type.clone())?;
        
        // Check if this is a function definition (has a body)
        if let Type::Function { .. } = full_type {
            if self.check(&TokenType::LeftBrace) {
                return Ok(TopLevelItem::Function(self.parse_function_definition(
                    name, full_type, storage_class
                )?));
            }
        }
        
        // Otherwise it's a declaration (possibly with multiple declarators)
        // If this is a typedef declaration, register the name
        if storage_class == StorageClass::Typedef {
            self.typedef_names.insert(name.clone());
        }
        
        // Parse optional initializer
        let initializer = if self.match_token(&TokenType::Equal) {
            Some(self.parse_initializer()?)
        } else {
            None
        };
        
        let first_decl = Declaration {
            node_id: self.node_id_gen.next(),
            name,
            decl_type: full_type,
            storage_class,
            initializer,
            span: SourceSpan::new(start_loc.clone(), self.current_location()),
            symbol_id: None,
        };
        
        // Check for additional declarators (comma-separated)
        if self.match_token(&TokenType::Comma) {
            let mut declarations = vec![first_decl];
            
            loop {
                let (name, full_type) = self.parse_declarator(base_type.clone())?;
                
                // If this is a typedef, register the name
                if storage_class == StorageClass::Typedef {
                    self.typedef_names.insert(name.clone());
                }
                
                let initializer = if self.match_token(&TokenType::Equal) {
                    Some(self.parse_initializer()?)
                } else {
                    None
                };
                
                declarations.push(Declaration {
                    node_id: self.node_id_gen.next(),
                    name,
                    decl_type: full_type,
                    storage_class,
                    initializer,
                    span: SourceSpan::new(start_loc.clone(), self.current_location()),
                    symbol_id: None,
                });
                
                if !self.match_token(&TokenType::Comma) {
                    break;
                }
            }
            
            self.expect(TokenType::Semicolon, "declaration")?;
            Ok(TopLevelItem::Declarations(declarations))
        } else {
            self.expect(TokenType::Semicolon, "declaration")?;
            Ok(TopLevelItem::Declarations(vec![first_decl]))
        }
    }
    
    /// Parse function definition
    pub fn parse_function_definition(
        &mut self,
        name: String,
        func_type: Type,
        storage_class: StorageClass,
    ) -> Result<FunctionDefinition, CompilerError> {
        let start_location = self.current_location();
        
        // Extract function type information
        let (return_type, _param_types) = match func_type {
            Type::Function { return_type, parameters, .. } => (*return_type, parameters),
            _ => return Err(ParseError::InvalidType {
                message: "Expected function type".to_string(),
                location: start_location,
            }.into()),
        };
        
        // Use the stored parameter information if available
        let parameters = if let Some(param_info) = self.last_function_params.take() {
            param_info.into_iter().map(|(name, param_type, span)| Parameter {
                node_id: self.node_id_gen.next(),
                name,
                param_type,
                span,
                symbol_id: None,
            }).collect()
        } else {
            Vec::new()
        };
        
        // Parse function body
        let body = self.parse_compound_statement()?;
        
        let end_location = self.current_location();
        
        Ok(FunctionDefinition {
            node_id: self.node_id_gen.next(),
            name,
            return_type,
            parameters,
            body,
            storage_class,
            span: SourceSpan::new(start_location, end_location),
            symbol_id: None,
        })
    }
    
    /// Parse declaration with known type information
    pub fn parse_declaration_with_type(
        &mut self,
        name: String,
        decl_type: Type,
        storage_class: StorageClass,
    ) -> Result<Declaration, CompilerError> {
        let start_location = self.current_location();
        
        // If this is a typedef declaration, register the name
        if storage_class == StorageClass::Typedef {
            self.typedef_names.insert(name.clone());
        }
        
        // Parse optional initializer
        let initializer = if self.match_token(&TokenType::Equal) {
            Some(self.parse_initializer()?)
        } else {
            None
        };
        
        self.expect(TokenType::Semicolon, "declaration")?;
        
        let end_location = self.current_location();
        
        Ok(Declaration {
            node_id: self.node_id_gen.next(),
            name,
            decl_type,
            storage_class,
            initializer,
            span: SourceSpan::new(start_location, end_location),
            symbol_id: None,
        })
    }
    
    /// Parse declaration statement
    pub fn parse_declaration_statement(&mut self) -> Result<StatementKind, CompilerError> {
        let storage_class = self.parse_storage_class();
        let base_type = self.parse_type_specifier()?;
        
        let mut declarations = Vec::new();
        
        // Parse first declarator
        let (name, full_type) = self.parse_declarator(base_type.clone())?;
        
        // If this is a typedef, register the name
        if storage_class == StorageClass::Typedef {
            self.typedef_names.insert(name.clone());
        }
        
        let initializer = if self.match_token(&TokenType::Equal) {
            Some(self.parse_initializer()?)
        } else {
            None
        };
        
        declarations.push(Declaration {
            node_id: self.node_id_gen.next(),
            name,
            decl_type: full_type,
            storage_class,
            initializer,
            span: SourceSpan::new(self.current_location(), self.current_location()),
            symbol_id: None,
        });
        
        // Parse additional declarators (comma-separated)
        while self.match_token(&TokenType::Comma) {
            let (name, full_type) = self.parse_declarator(base_type.clone())?;
            
            // If this is a typedef, register the name
            if storage_class == StorageClass::Typedef {
                self.typedef_names.insert(name.clone());
            }
            
            let initializer = if self.match_token(&TokenType::Equal) {
                Some(self.parse_initializer()?)
            } else {
                None
            };
            
            declarations.push(Declaration {
                node_id: self.node_id_gen.next(),
                name,
                decl_type: full_type,
                storage_class,
                initializer,
                span: SourceSpan::new(self.current_location(), self.current_location()),
                symbol_id: None,
            });
        }
        
        self.expect(TokenType::Semicolon, "declaration")?;
        
        Ok(StatementKind::Declaration { declarations })
    }
    
    /// Parse initializer
    pub fn parse_initializer(&mut self) -> Result<Initializer, CompilerError> {
        let start_location = self.current_location();
        
        let kind = if self.match_token(&TokenType::LeftBrace) {
            // Parse initializer list (which may contain designated initializers)
            let mut initializers = Vec::new();
            
            if !self.check(&TokenType::RightBrace) {
                loop {
                    // Check for designator (.field or [index]), including nested
                    // chains such as `.data.ival = 100`.
                    if self.check(&TokenType::Dot) || self.check(&TokenType::LeftBracket) {
                        initializers.push(self.parse_designated_initializer()?);
                    } else {
                        // Regular initializer (no designator)
                        initializers.push(self.parse_initializer()?);
                    }
                    
                    if !self.match_token(&TokenType::Comma) {
                        break;
                    }
                    
                    // Allow trailing comma
                    if self.check(&TokenType::RightBrace) {
                        break;
                    }
                }
            }
            
            self.expect(TokenType::RightBrace, "initializer list")?;
            InitializerKind::List(initializers)
        } else {
            // Expression initializer
            let expr = self.parse_assignment_expression()?;
            InitializerKind::Expression(expr)
        };
        
        let end_location = self.current_location();
        
        Ok(Initializer {
            node_id: self.node_id_gen.next(),
            kind,
            span: SourceSpan::new(start_location, end_location),
        })
    }

    /// Parse a (possibly nested) designated initializer: `.field`, `[index]`, or `.a.b = value`.
    fn parse_designated_initializer(&mut self) -> Result<Initializer, CompilerError> {
        let start_location = self.current_location();
        let designator = self.parse_designator()?;

        let inner = if self.check(&TokenType::Dot) || self.check(&TokenType::LeftBracket) {
            self.parse_designated_initializer()?
        } else {
            self.expect(TokenType::Equal, "designated initializer")?;
            self.parse_initializer()?
        };

        Ok(Initializer {
            node_id: self.node_id_gen.next(),
            kind: InitializerKind::Designated {
                designator,
                initializer: Box::new(inner),
            },
            span: SourceSpan::new(start_location, self.current_location()),
        })
    }

    fn parse_designator(&mut self) -> Result<crate::ast::Designator, CompilerError> {
        if self.match_token(&TokenType::Dot) {
            let ident = if let Some(Token { token_type: TokenType::Identifier(name), .. }) = self.peek() {
                let name = name.clone();
                self.advance();
                name
            } else {
                return Err(CompilerError::parse_error(
                    "Expected field name after '.'".to_string(),
                    self.current_location(),
                ));
            };
            Ok(crate::ast::Designator::Member(ident))
        } else if self.match_token(&TokenType::LeftBracket) {
            let index_expr = self.parse_expression()?;
            self.expect(TokenType::RightBracket, "array designator")?;
            Ok(crate::ast::Designator::Index(index_expr))
        } else {
            Err(CompilerError::parse_error(
                "Expected '.' or '[' for designator".to_string(),
                self.current_location(),
            ))
        }
    }
}