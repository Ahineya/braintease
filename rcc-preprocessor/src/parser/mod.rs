use crate::lexer::{Token, TokenType};
use anyhow::{anyhow, Result};

#[derive(Debug, Clone)]
pub enum Directive {
    Include {
        operand: String,
    },
    Define {
        name: String,
        params: Option<Vec<String>>,
        body: String,
        is_variadic: bool,
    },
    Undef {
        name: String,
    },
    If {
        condition: String,
    },
    Ifdef {
        name: String,
    },
    Ifndef {
        name: String,
    },
    Elif {
        condition: String,
    },
    Else,
    Endif,
    Line {
        number: usize,
        file: Option<String>,
    },
    Pragma {
        content: String,
    },
    Error {
        message: String,
    },
    Warning {
        message: String,
    },
    // Definition syntax is only diagnosed when its conditional group executes.
    InvalidDefinition { message: String },
    Text(String),
}

pub struct Parser {
    tokens: Vec<Token>,
    current: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Self { tokens, current: 0 }
    }
    
    pub fn parse(&mut self) -> Result<Vec<Directive>> {
        let mut directives = Vec::new();
        let mut current_text = String::new();
        
        while !self.is_at_end() {
            match &self.peek().token_type {
                TokenType::Hash | TokenType::Include | TokenType::Define | TokenType::Undef |
                TokenType::If | TokenType::Ifdef | TokenType::Ifndef | TokenType::Elif |
                TokenType::Else | TokenType::Endif | TokenType::Line | TokenType::Pragma |
                TokenType::Error | TokenType::Warning => {
                    // Flush any accumulated text
                    if !current_text.is_empty() {
                        directives.push(Directive::Text(current_text.clone()));
                        current_text.clear();
                    }
                    
                    // Parse the directive
                    directives.push(self.parse_directive()?);
                }
                TokenType::Newline => {
                    current_text.push('\n');
                    self.advance();
                }
                TokenType::Whitespace(ws) => {
                    current_text.push_str(ws);
                    self.advance();
                }
                TokenType::Comment(comment) => {
                    current_text.push_str(comment);
                    self.advance();
                }
                TokenType::Eof => {
                    break;
                }
                _ => {
                    // Regular text
                    current_text.push_str(&self.peek().text);
                    self.advance();
                }
            }
        }
        
        // Flush any remaining text
        if !current_text.is_empty() {
            directives.push(Directive::Text(current_text));
        }
        
        Ok(directives)
    }
    
    fn parse_directive(&mut self) -> Result<Directive> {
        let token = self.advance();
        
        match &token.token_type {
            TokenType::Include => self.parse_include(),
            TokenType::Define => match self.parse_define() {
                Ok(definition) => Ok(definition),
                Err(error) => {
                    self.skip_to_newline();
                    Ok(Directive::InvalidDefinition { message: error.to_string() })
                }
            },
            TokenType::Undef => self.parse_undef(),
            TokenType::If => self.parse_if(),
            TokenType::Ifdef => self.parse_ifdef(),
            TokenType::Ifndef => self.parse_ifndef(),
            TokenType::Elif => self.parse_elif(),
            TokenType::Else => {
                self.skip_to_newline();
                Ok(Directive::Else)
            }
            TokenType::Endif => {
                self.skip_to_newline();
                Ok(Directive::Endif)
            }
            TokenType::Line => self.parse_line(),
            TokenType::Pragma => self.parse_pragma(),
            TokenType::Error => self.parse_error(),
            TokenType::Warning => self.parse_warning(),
            TokenType::Hash => {
                // Standalone # or unknown directive
                self.skip_to_newline();
                Ok(Directive::Text("#".to_string()))
            }
            _ => Err(anyhow!("Unexpected token in directive: {:?}", token)),
        }
    }
    
    fn parse_include(&mut self) -> Result<Directive> {
        // Resolve only when this directive executes: macros may be defined by
        // earlier includes, and skipped conditional groups must not expand it.
        let operand = self.collect_line()?;
        self.skip_to_newline();
        Ok(Directive::Include { operand })
    }
    
    fn parse_define(&mut self) -> Result<Directive> {
        self.skip_whitespace();
        
        // Get macro name
        let name = if let TokenType::Identifier(name) = &self.peek().token_type {
            let name = name.clone();
            self.advance();
            name
        } else {
            return Err(anyhow!("Expected identifier in define directive"));
        };
        
        // Check for function-like macro
        let (params, is_variadic) = if self.check_no_space(&TokenType::LeftParen) {
            self.advance();
            self.parse_macro_params()?
        } else {
            (None, false)
        };
        
        // Get macro body (rest of the line, handling continuations)
        let body = self.collect_macro_body()?;
        
        Ok(Directive::Define {
            name,
            params,
            body,
            is_variadic,
        })
    }
    
    fn parse_macro_params(&mut self) -> Result<(Option<Vec<String>>, bool)> {
        let mut params = Vec::new();
        let mut is_variadic = false;
        
        self.skip_whitespace();
        
        if self.check(&TokenType::RightParen) {
            self.advance();
            return Ok((Some(params), false));
        }
        
        loop {
            self.skip_whitespace();
            
            if self.check(&TokenType::Ellipsis) {
                self.advance();
                is_variadic = true;
                self.skip_whitespace();
                if !self.check(&TokenType::RightParen) {
                    return Err(anyhow!("Expected ')' after '...' in macro parameters"));
                }
                self.advance();
                break;
            }
            
            if let TokenType::Identifier(name) = &self.peek().token_type {
                params.push(name.clone());
                self.advance();
            } else {
                return Err(anyhow!("Expected parameter name in macro definition"));
            }
            
            self.skip_whitespace();
            
            if self.check(&TokenType::RightParen) {
                self.advance();
                break;
            }
            
            if !self.check(&TokenType::Comma) {
                return Err(anyhow!("Expected ',' or ')' in macro parameters"));
            }
            self.advance();
        }
        
        Ok((Some(params), is_variadic))
    }
    
    fn collect_macro_body(&mut self) -> Result<String> {
        let mut body = String::new();
        
        // Skip any initial whitespace
        self.skip_whitespace();
        
        while !self.is_at_end() {
            if self.check(&TokenType::Newline) {
                // Check for line continuation
                let saved_pos = self.current;
                self.advance(); // consume newline
                
                // Look back to see if there was a backslash
                if body.ends_with('\\') {
                    // Remove the backslash and continue
                    body.truncate(body.len() - 1);
                    body.push(' '); // Replace with space
                } else {
                    // End of macro body
                    self.current = saved_pos;
                    break;
                }
            } else {
                // Skip comments but include everything else
                let token = self.peek();
                match &token.token_type {
                    TokenType::Comment(_) => {
                        // Translation phase 3 replaces each comment with space.
                        body.push(' ');
                        self.advance();
                    }
                    _ => {
                        body.push_str(&self.advance().text);
                    }
                }
            }
        }
        
        Ok(body.trim().to_string())
    }
    
    fn parse_undef(&mut self) -> Result<Directive> {
        self.skip_whitespace();
        
        let name = if let TokenType::Identifier(name) = &self.peek().token_type {
            let name = name.clone();
            self.advance();
            name
        } else {
            return Err(anyhow!("Expected identifier in undef directive"));
        };
        
        self.skip_to_newline();
        Ok(Directive::Undef { name })
    }
    
    fn parse_if(&mut self) -> Result<Directive> {
        let condition = self.collect_line()?;
        Ok(Directive::If { condition })
    }
    
    fn parse_ifdef(&mut self) -> Result<Directive> {
        self.skip_whitespace();
        
        let name = if let TokenType::Identifier(name) = &self.peek().token_type {
            let name = name.clone();
            self.advance();
            name
        } else {
            return Err(anyhow!("Expected identifier in ifdef directive"));
        };
        
        self.skip_to_newline();
        Ok(Directive::Ifdef { name })
    }
    
    fn parse_ifndef(&mut self) -> Result<Directive> {
        self.skip_whitespace();
        
        let name = if let TokenType::Identifier(name) = &self.peek().token_type {
            let name = name.clone();
            self.advance();
            name
        } else {
            return Err(anyhow!("Expected identifier in ifndef directive"));
        };
        
        self.skip_to_newline();
        Ok(Directive::Ifndef { name })
    }
    
    fn parse_elif(&mut self) -> Result<Directive> {
        let condition = self.collect_line()?;
        Ok(Directive::Elif { condition })
    }
    
    fn parse_line(&mut self) -> Result<Directive> {
        self.skip_whitespace();
        
        let number = if let TokenType::Number(n) = &self.peek().token_type {
            let num = n.parse::<usize>().map_err(|_| anyhow!("Invalid line number"))?;
            self.advance();
            num
        } else {
            return Err(anyhow!("Expected line number in line directive"));
        };
        
        self.skip_whitespace();
        
        let file = if let TokenType::StringLiteral(f) = &self.peek().token_type {
            let file = f.clone();
            self.advance();
            Some(file)
        } else {
            None
        };
        
        self.skip_to_newline();
        Ok(Directive::Line { number, file })
    }
    
    fn parse_pragma(&mut self) -> Result<Directive> {
        let content = self.collect_line()?;
        Ok(Directive::Pragma { content })
    }
    
    fn parse_error(&mut self) -> Result<Directive> {
        let message = self.collect_line()?;
        Ok(Directive::Error { message })
    }
    
    fn parse_warning(&mut self) -> Result<Directive> {
        let message = self.collect_line()?;
        Ok(Directive::Warning { message })
    }
    
    fn collect_line(&mut self) -> Result<String> {
        let mut line = String::new();
        
        self.skip_whitespace();
        
        while !self.is_at_end() && !self.check(&TokenType::Newline) {
            let token = self.advance();
            if matches!(token.token_type, TokenType::Comment(_)) {
                line.push(' ');
            } else {
                line.push_str(&token.text);
            }
        }
        
        Ok(line.trim().to_string())
    }
    
    fn skip_whitespace(&mut self) {
        while !self.is_at_end() {
            if let TokenType::Whitespace(_) = &self.peek().token_type {
                self.advance();
            } else {
                break;
            }
        }
    }
    
    fn skip_to_newline(&mut self) {
        while !self.is_at_end() && !self.check(&TokenType::Newline) {
            self.advance();
        }
        if self.check(&TokenType::Newline) {
            self.advance();
        }
    }
    
    fn check(&self, token_type: &TokenType) -> bool {
        if self.is_at_end() {
            false
        } else {
            std::mem::discriminant(&self.peek().token_type) == std::mem::discriminant(token_type)
        }
    }
    
    fn check_no_space(&self, token_type: &TokenType) -> bool {
        // Check if the next token is of the given type without any space before it
        if self.is_at_end() {
            return false;
        }
        
        // For function-like macros, the '(' must immediately follow the name
        if self.current > 0 {
            let prev = &self.tokens[self.current - 1];
            let curr = &self.tokens[self.current];
            
            // Check if there's no space between tokens (they're adjacent in the source)
            if prev.column + prev.text.len() == curr.column {
                return self.check(token_type);
            }
        }
        
        false
    }
    
    fn peek(&self) -> &Token {
        &self.tokens[self.current]
    }
    
    fn advance(&mut self) -> Token {
        if !self.is_at_end() {
            self.current += 1;
        }
        self.tokens[self.current - 1].clone()
    }
    
    fn is_at_end(&self) -> bool {
        matches!(self.peek().token_type, TokenType::Eof)
    }
}

/// Convenience function to parse tokens
pub fn parse(tokens: &[Token]) -> Result<Vec<Directive>> {
    let mut parser = Parser::new(tokens.to_vec());
    parser.parse()
}
