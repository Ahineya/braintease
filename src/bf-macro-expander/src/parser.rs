use crate::ast::*;
use crate::lexer::{Token, TokenType};
use crate::types::{MacroExpansionError, MacroExpansionErrorType, MacroToken, MacroTokenType, Range, SourceLocation};

pub struct ParseResult {
    pub ast: ProgramNode,
    pub errors: Vec<MacroExpansionError>,
    pub tokens: Vec<MacroToken>,
}

pub struct Parser {
    tokens: Vec<Token>,
    current: usize,
    errors: Vec<MacroExpansionError>,
    macro_tokens: Vec<MacroToken>,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Self {
            tokens,
            current: 0,
            errors: Vec::new(),
            macro_tokens: Vec::new(),
        }
    }
    
    pub fn parse(&mut self) -> ParseResult {
        let mut statements = Vec::new();
        
        while !self.is_at_end() {
            // Skip newlines at statement level
            while self.match_token(TokenType::Newline) {
                // Continue
            }
            
            if self.is_at_end() {
                break;
            }
            
            if let Some(stmt) = self.parse_statement() {
                statements.push(stmt);
            }
        }
        
        let end_pos = if !self.tokens.is_empty() {
            self.tokens[self.tokens.len() - 1].position.end
        } else {
            0
        };
        
        let ast = ProgramNode {
            statements,
            position: ASTPosition {
                start: 0,
                end: end_pos,
                line: 1,
                column: 1,
            },
        };
        
        ParseResult {
            ast,
            errors: self.errors.clone(),
            tokens: self.macro_tokens.clone(),
        }
    }
    
    fn parse_statement(&mut self) -> Option<StatementNode> {
        let saved_position = self.current;
        
        // Skip whitespace to check for #define
        self.skip_whitespace();
        
        if self.check(TokenType::Define) {
            return self.parse_macro_definition();
        }
        
        // Backtrack to preserve leading whitespace
        self.current = saved_position;
        self.parse_code_line()
    }
    
    fn parse_macro_definition(&mut self) -> Option<StatementNode> {
        let start = self.peek().position.start;
        let start_line = self.peek().position.line;
        let start_column = self.peek().position.column;
        
        self.consume(TokenType::Define);
        self.skip_whitespace();
        
        if !self.check(TokenType::Identifier) {
            let pos = self.peek().position.clone();
            self.add_error("Expected macro name after #define", pos);
            self.synchronize();
            return None;
        }
        
        let name = self.advance().value.clone();
        let current_pos = if self.current < self.tokens.len() {
            self.peek().position.start
        } else {
            self.tokens[self.current - 1].position.end
        };
        
        // Add macro definition token
        self.macro_tokens.push(MacroToken {
            token_type: MacroTokenType::MacroDefinition,
            range: Range {
                start,
                end: current_pos,
            },
            name: name.clone(),
        });
        
        let mut parameters = None;
        
        // Check for parameters
        if self.check(TokenType::LParen) {
            self.advance(); // consume (
            parameters = Some(self.parse_parameter_list());
            if !self.consume(TokenType::RParen) {
                let pos = self.peek().position.clone();
                self.add_error("Expected ')' after parameter list", pos);
            }
        }
        
        self.skip_whitespace();
        
        // Check if this is a brace-style multiline macro
        let body = if self.check(TokenType::LBrace) {
            self.parse_brace_macro_body()
        } else {
            // Parse body (everything until newline, handling line continuations)
            self.parse_macro_body()
        };
        
        let end_pos = if !body.is_empty() {
            match body.last().unwrap() {
                ContentNode::BrainfuckCommand(node) => node.position.end,
                ContentNode::MacroInvocation(node) => node.position.end,
                ContentNode::BuiltinFunction(node) => node.position.end,
                ContentNode::Text(node) => node.position.end,
            }
        } else if self.current > 0 {
            self.tokens[self.current.saturating_sub(1)].position.end
        } else {
            start
        };
        
        Some(StatementNode::MacroDefinition(MacroDefinitionNode {
            name,
            parameters,
            body,
            position: ASTPosition {
                start,
                end: end_pos,
                line: start_line,
                column: start_column,
            },
        }))
    }
    
    fn parse_parameter_list(&mut self) -> Vec<String> {
        let mut params = Vec::new();
        
        self.skip_whitespace();
        
        if !self.check(TokenType::RParen) {
            loop {
                self.skip_whitespace();
                
                if self.check(TokenType::Identifier) {
                    params.push(self.advance().value.clone());
                } else {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected parameter name", pos);
                    break;
                }
                
                self.skip_whitespace();
                
                if !self.match_token(TokenType::Comma) {
                    break;
                }
            }
        }
        
        params
    }
    
    fn parse_macro_body(&mut self) -> Vec<BodyNode> {
        let mut body = Vec::new();
        let mut has_line_continuation = true;
        
        while has_line_continuation && !self.is_at_end() {
            has_line_continuation = false;
            
            // Parse content until newline or backslash
            while !self.is_at_end() && !self.check(TokenType::Newline) && !self.check(TokenType::Backslash) {
                if let Some(content) = self.parse_content() {
                    body.push(content);
                }
            }
            
            // Check for line continuation
            if self.check(TokenType::Backslash) {
                self.advance(); // consume backslash
                // The lexer already handles \n after backslash, so we just continue
                has_line_continuation = true;
                
                // Skip any whitespace after the line continuation
                self.skip_whitespace();
            }
        }
        
        // Consume the final newline if present
        self.match_token(TokenType::Newline);
        
        body
    }
    
    fn parse_brace_macro_body(&mut self) -> Vec<BodyNode> {
        let mut body = Vec::new();
        
        // Consume the opening brace
        self.consume(TokenType::LBrace);
        
        // Skip any whitespace or newlines after the opening brace
        self.skip_whitespace_and_newlines();
        
        // Track brace depth for nested structures
        let mut brace_depth = 1;
        
        while !self.is_at_end() && brace_depth > 0 {
            // Skip whitespace and newlines at the beginning of each iteration
            while self.match_token(TokenType::Whitespace) || self.match_token(TokenType::Newline) {
                // Continue
            }
            
            // Skip comments
            if self.match_token(TokenType::Comment) {
                continue;
            }
            
            // Check for closing brace
            if self.check(TokenType::RBrace) {
                brace_depth -= 1;
                if brace_depth == 0 {
                    // Consume the final closing brace
                    self.advance();
                    break;
                }
                // It's a nested closing brace, include it as text
                let token = self.advance();
                body.push(ContentNode::Text(TextNode {
                    value: token.value.clone(),
                    position: token.position.clone(),
                }));
            } else if self.check(TokenType::LBrace) {
                // Check if it's a builtin function
                let saved_position = self.current;
                if let Some(content) = self.parse_content() {
                    if matches!(content, ContentNode::BuiltinFunction(_)) {
                        // It was parsed as a builtin function, add it
                        body.push(content);
                    } else {
                        // It's a standalone opening brace
                        // Reset position and consume as text
                        self.current = saved_position;
                        let token = self.advance();
                        body.push(ContentNode::Text(TextNode {
                            value: token.value.clone(),
                            position: token.position.clone(),
                        }));
                        brace_depth += 1;
                    }
                } else {
                    // Reset and consume as text
                    self.current = saved_position;
                    let token = self.advance();
                    body.push(ContentNode::Text(TextNode {
                        value: token.value.clone(),
                        position: token.position.clone(),
                    }));
                    brace_depth += 1;
                }
            } else {
                // Parse regular content
                if let Some(content) = self.parse_content() {
                    body.push(content);
                }
            }
        }
        
        if brace_depth > 0 {
            let pos = self.peek().position.clone();
            self.add_error("Unclosed macro body - missing closing brace }", pos);
        }
        
        body
    }
    
    fn parse_code_line(&mut self) -> Option<StatementNode> {
        let start = self.peek().position.start;
        let start_line = self.peek().position.line;
        let start_column = self.peek().position.column;
        
        let mut content = Vec::new();
        
        while !self.is_at_end() && !self.check(TokenType::Newline) {
            if let Some(node) = self.parse_content() {
                content.push(node);
            } else {
                self.advance(); // Skip unknown tokens
            }
        }
        
        // Consume the newline if present
        self.match_token(TokenType::Newline);
        
        let end_pos = if !content.is_empty() {
            match content.last().unwrap() {
                ContentNode::BrainfuckCommand(node) => node.position.end,
                ContentNode::MacroInvocation(node) => node.position.end,
                ContentNode::BuiltinFunction(node) => node.position.end,
                ContentNode::Text(node) => node.position.end,
            }
        } else {
            start
        };
        
        Some(StatementNode::CodeLine(CodeLineNode {
            content,
            position: ASTPosition {
                start,
                end: end_pos,
                line: start_line,
                column: start_column,
            },
        }))
    }
    
    fn parse_content(&mut self) -> Option<ContentNode> {
        // Skip comments and whitespace that should be ignored
        while self.check(TokenType::Comment) || 
              (self.check(TokenType::Whitespace) && self.peek_ahead(1).map_or(false, |t| 
                  matches!(t.token_type, TokenType::Newline | TokenType::Eof))) {
            self.advance();
        }
        
        if self.is_at_end() || self.check(TokenType::Newline) {
            return None;
        }
        
        match self.peek().token_type {
            TokenType::BrainfuckCommand => {
                let token = self.advance();
                Some(ContentNode::BrainfuckCommand(BrainfuckCommandNode {
                    commands: token.value.clone(),
                    position: token.position.clone(),
                }))
            }
            TokenType::At | TokenType::Hash => {
                self.advance(); // consume @ or #
                self.parse_macro_invocation().map(ContentNode::MacroInvocation)
            }
            TokenType::LBrace => {
                // Check if it's a builtin function
                let saved_position = self.current;
                self.advance(); // consume {
                if self.check(TokenType::Identifier) {
                    let name = self.peek().value.clone();
                    if matches!(name.as_str(), "repeat" | "if" | "for" | "reverse" | "preserve" | "proc" | "local" | "len") {
                        if let Some(builtin) = self.parse_builtin_function() {
                            return Some(ContentNode::BuiltinFunction(builtin));
                        }
                    }
                }
                // Otherwise it's just a regular brace - return it as text
                self.current = saved_position;
                let token = self.advance();
                Some(ContentNode::Text(TextNode {
                    value: token.value.clone(),
                    position: token.position.clone(),
                }))
            }
            TokenType::RBrace | TokenType::Comma | TokenType::LParen | TokenType::RParen => {
                // Return these as text nodes
                let token = self.advance();
                Some(ContentNode::Text(TextNode {
                    value: token.value.clone(),
                    position: token.position.clone(),
                }))
            }
            TokenType::BuiltinRepeat | TokenType::BuiltinIf | TokenType::BuiltinFor | TokenType::BuiltinReverse | TokenType::BuiltinPreserve | TokenType::BuiltinLabel | TokenType::BuiltinBr | TokenType::BuiltinProc | TokenType::BuiltinLocal | TokenType::BuiltinLen | TokenType::ColonShorthand => {
                self.parse_builtin_function().map(ContentNode::BuiltinFunction)
            }
            TokenType::Whitespace | TokenType::Text | TokenType::Identifier | TokenType::Number => {
                // Combine consecutive text-like tokens into a single text node
                let start_token = self.advance().clone();
                let mut combined_text = start_token.value.clone();
                let start_pos = start_token.position.start;
                let mut end_pos = start_token.position.end;
                let start_line = start_token.position.line;
                let start_column = start_token.position.column;
                
                // Keep combining consecutive text-like tokens (but not newlines)
                while !self.is_at_end() && !self.check(TokenType::Newline) {
                    match self.peek().token_type {
                        TokenType::Text | TokenType::Identifier | TokenType::Number | TokenType::Whitespace => {
                            let next_token = self.advance().clone();
                            combined_text.push_str(&next_token.value);
                            end_pos = next_token.position.end;
                        }
                        _ => break,
                    }
                }
                
                Some(ContentNode::Text(TextNode {
                    value: combined_text,
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                }))
            }
            _ => {
                self.advance();
                None
            }
        }
    }
    
    fn parse_macro_invocation(&mut self) -> Option<MacroInvocationNode> {
        let start_pos = self.tokens[self.current.saturating_sub(1)].position.start;
        let start_line = self.tokens[self.current.saturating_sub(1)].position.line;
        let start_column = self.tokens[self.current.saturating_sub(1)].position.column;
        
        if !self.check(TokenType::Identifier) {
            let pos = self.peek().position.clone();
            self.add_error("Expected macro name after @", pos);
            return None;
        }
        
        let name = self.advance().value.clone();
        let mut arguments = None;
        
        if self.check(TokenType::LParen) {
            self.advance(); // consume (
            arguments = Some(self.parse_argument_list());
            if !self.consume(TokenType::RParen) {
                let pos = self.peek().position.clone();
                self.add_error("Expected ')' after arguments", pos);
            }
        }
        
        let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
        
        // Add macro invocation token
        self.macro_tokens.push(MacroToken {
            token_type: MacroTokenType::MacroInvocation,
            range: Range {
                start: start_pos,
                end: end_pos,
            },
            name: name.clone(),
        });
        
        Some(MacroInvocationNode {
            name,
            arguments,
            position: ASTPosition {
                start: start_pos,
                end: end_pos,
                line: start_line,
                column: start_column,
            },
        })
    }
    
    fn parse_builtin_function(&mut self) -> Option<BuiltinFunctionNode> {
        let start_pos = self.tokens[self.current.saturating_sub(1)].position.start;
        let start_line = self.tokens[self.current.saturating_sub(1)].position.line;
        let start_column = self.tokens[self.current.saturating_sub(1)].position.column;
        
        let (name_str, name) = match self.peek().token_type {
            TokenType::BuiltinRepeat => {
                self.advance();
                ("repeat".to_string(), BuiltinFunction::Repeat)
            }
            TokenType::BuiltinIf => {
                self.advance();
                ("if".to_string(), BuiltinFunction::If)
            }
            TokenType::BuiltinFor => {
                self.advance();
                ("for".to_string(), BuiltinFunction::For)
            }
            TokenType::BuiltinReverse => {
                self.advance();
                ("reverse".to_string(), BuiltinFunction::Reverse)
            }
            TokenType::BuiltinPreserve => {
                self.advance();
                ("preserve".to_string(), BuiltinFunction::Preserve)
            }
            TokenType::BuiltinLabel => {
                // {label doesn't require parentheses, collect content until }
                self.advance();
                
                // Collect tokens until we hit the closing brace
                let mut label_name = String::new();
                while !self.is_at_end() && !self.check(TokenType::RBrace) {
                    let token = self.advance();
                    label_name.push_str(&token.value);
                }
                
                // Consume the closing brace
                if !self.consume(TokenType::RBrace) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '}' after label name", pos);
                }
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                // Create label node with the name as an argument
                // Use a safe position reference
                let arg_position = if start_pos < self.tokens.len() {
                    self.tokens[start_pos].position.clone()
                } else {
                    ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    }
                };
                
                let label_arg = ExpressionNode::Text(TextNode {
                    value: label_name.trim().to_string(),
                    position: arg_position,
                });
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Label,
                    arguments: vec![label_arg],
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::BuiltinBr => {
                // {br doesn't require parentheses or arguments
                self.advance();
                
                // Consume the closing brace
                if !self.consume(TokenType::RBrace) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '}' after br", pos);
                }
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Br,
                    arguments: vec![],
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::BuiltinProc => {
                self.advance();
                
                // Expect opening parenthesis
                if !self.consume(TokenType::LParen) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '(' after proc", pos);
                    return None;
                }
                
                // Parse the body as a sequence of content nodes (like macro body)
                let mut body_nodes = Vec::new();
                let mut paren_depth = 1;
                
                // Skip initial whitespace/newlines
                while self.check(TokenType::Whitespace) || self.check(TokenType::Newline) {
                    self.advance();
                }
                
                // Collect body content until matching closing paren
                while !self.is_at_end() && paren_depth > 0 {
                    if self.check(TokenType::LParen) {
                        paren_depth += 1;
                        let token = self.advance();
                        body_nodes.push(ContentNode::Text(TextNode {
                            value: token.value.clone(),
                            position: token.position.clone(),
                        }));
                    } else if self.check(TokenType::RParen) {
                        paren_depth -= 1;
                        if paren_depth == 0 {
                            self.advance(); // consume closing paren
                            break;
                        }
                        let token = self.advance();
                        body_nodes.push(ContentNode::Text(TextNode {
                            value: token.value.clone(),
                            position: token.position.clone(),
                        }));
                    } else if let Some(content) = self.parse_content() {
                        body_nodes.push(content);
                    } else {
                        // If parse_content returns None, advance and treat as text
                        let token = self.advance();
                        body_nodes.push(ContentNode::Text(TextNode {
                            value: token.value.clone(),
                            position: token.position.clone(),
                        }));
                    }
                }
                
                if paren_depth > 0 {
                    let pos = self.peek().position.clone();
                    self.add_error("Unclosed proc body - missing ')'", pos);
                }
                
                // Expect closing brace
                if !self.consume(TokenType::RBrace) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '}' after proc", pos);
                }
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                // Wrap the body nodes in an ExpressionList
                let body_expr = ExpressionNode::ExpressionList(ExpressionListNode {
                    expressions: body_nodes,
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Proc,
                    arguments: vec![body_expr],
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::BuiltinLocal => {
                self.advance();
                
                let mut arguments = Vec::new();
                
                // Skip initial whitespace
                self.skip_whitespace();
                
                // Parse the variable name (with or without $)
                let start_arg = self.current;
                let mut var_name = String::new();
                
                // Collect the variable name (including $ prefix)
                while !self.is_at_end() && !self.check(TokenType::RBrace) && !self.check(TokenType::Whitespace) && !self.check(TokenType::Number) {
                    let token = self.advance();
                    var_name.push_str(&token.value);
                }
                
                if var_name.is_empty() {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected variable name after {local", pos);
                    return None;
                }
                
                let var_pos = if start_arg < self.tokens.len() {
                    self.tokens[start_arg].position.clone()
                } else {
                    ASTPosition {
                        start: start_pos,
                        end: start_pos,
                        line: start_line,
                        column: start_column,
                    }
                };
                
                arguments.push(ExpressionNode::Text(TextNode {
                    value: var_name,
                    position: var_pos,
                }));
                
                // Check for optional size (number)
                self.skip_whitespace();
                if self.check(TokenType::Number) {
                    let size_token = self.advance();
                    arguments.push(ExpressionNode::Number(NumberNode {
                        value: size_token.value.parse().unwrap_or(1),
                        position: size_token.position.clone(),
                    }));
                }
                
                // Expect closing brace
                if !self.consume(TokenType::RBrace) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '}' after local", pos);
                }
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Local,
                    arguments,
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::BuiltinLen => {
                self.advance();
                
                let mut arguments = Vec::new();
                
                // Skip initial whitespace
                self.skip_whitespace();
                
                // Parse the variable name
                let mut var_name = String::new();
                while !self.is_at_end() && !self.check(TokenType::RBrace) {
                    let token = self.advance();
                    var_name.push_str(&token.value);
                }
                
                if var_name.is_empty() {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected variable name after {len", pos);
                    return None;
                }
                
                arguments.push(ExpressionNode::Text(TextNode {
                    value: var_name.trim().to_string(),
                    position: ASTPosition {
                        start: start_pos,
                        end: start_pos,
                        line: start_line,
                        column: start_column,
                    },
                }));
                
                // Expect closing brace
                if !self.consume(TokenType::RBrace) {
                    let pos = self.peek().position.clone();
                    self.add_error("Expected '}' after len", pos);
                }
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Len,
                    arguments,
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::ColonShorthand => {
                // {: shorthand doesn't use parentheses, content goes directly until }
                self.advance();
                let start_content = self.current;
                let mut content = Vec::new();
                let mut brace_depth = 1;
                
                // Collect everything until the matching closing brace
                while !self.is_at_end() && brace_depth > 0 {
                    if self.check(TokenType::RBrace) {
                        brace_depth -= 1;
                        if brace_depth == 0 {
                            self.advance(); // consume the closing brace
                            break;
                        }
                    } else if self.check(TokenType::LBrace) {
                        brace_depth += 1;
                    }
                    
                    // Collect the token value as preserved text
                    let token = self.advance();
                    content.push(token.value.clone());
                }
                
                // Create a text expression with the preserved content
                let preserved_text = content.join("");
                let text_node = ExpressionNode::Text(TextNode {
                    value: preserved_text,
                    position: self.tokens[start_content].position.clone(),
                });
                
                let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
                
                // Add builtin function token
                self.macro_tokens.push(MacroToken {
                    token_type: MacroTokenType::BuiltinFunction,
                    range: Range {
                        start: start_pos,
                        end: end_pos,
                    },
                    name: "preserve".to_string(),
                });
                
                return Some(BuiltinFunctionNode {
                    name: BuiltinFunction::Preserve,
                    arguments: vec![text_node],
                    position: ASTPosition {
                        start: start_pos,
                        end: end_pos,
                        line: start_line,
                        column: start_column,
                    },
                });
            }
            TokenType::Identifier => {
                // Handle when we're coming from a regular { followed by identifier
                let name_str = self.advance().value.clone();
                let name = match name_str.as_str() {
                    "repeat" => BuiltinFunction::Repeat,
                    "if" => BuiltinFunction::If,
                    "for" => BuiltinFunction::For,
                    "reverse" => BuiltinFunction::Reverse,
                    "preserve" => BuiltinFunction::Preserve,
                    "label" => BuiltinFunction::Label,
                    "br" => BuiltinFunction::Br,
                    "proc" => BuiltinFunction::Proc,
                    "local" => BuiltinFunction::Local,
                    "len" => BuiltinFunction::Len,
                    _ => {
                        let pos = self.peek().position.clone();
                        self.add_error(&format!("Unknown builtin function: {}", name_str), pos);
                        return None;
                    }
                };
                (name_str, name)
            }
            _ => {
                let pos = self.peek().position.clone();
                self.add_error("Expected builtin function name", pos);
                return None;
            }
        };
        
        // Preserve with explicit {preserve(...)} still requires parentheses
        if !self.consume(TokenType::LParen) {
            let pos = self.peek().position.clone();
            self.add_error("Expected '(' after function name", pos);
            return None;
        }
        
        let arguments = if name == BuiltinFunction::For {
            self.parse_for_arguments()
        } else if name == BuiltinFunction::Preserve {
            // For preserve, we need to keep exact content including whitespace
            self.parse_preserve_arguments()
        } else {
            self.parse_argument_list()
        };
        
        if !self.consume(TokenType::RParen) {
            let pos = self.peek().position.clone();
            self.add_error("Expected ')' after arguments", pos);
        }
        
        if !self.consume(TokenType::RBrace) {
            let pos = self.peek().position.clone();
            self.add_error("Expected '}' after builtin function", pos);
        }
        
        let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
        
        // Add builtin function token
        self.macro_tokens.push(MacroToken {
            token_type: MacroTokenType::BuiltinFunction,
            range: Range {
                start: start_pos,
                end: end_pos,
            },
            name: name_str,
        });
        
        Some(BuiltinFunctionNode {
            name,
            arguments,
            position: ASTPosition {
                start: start_pos,
                end: end_pos,
                line: start_line,
                column: start_column,
            },
        })
    }
    
    fn parse_preserve_arguments(&mut self) -> Vec<ExpressionNode> {
        // For preserve, collect everything until the closing paren as-is
        let start_pos = self.current;
        let mut content = Vec::new();
        let mut paren_depth = 0;
        
        while !self.is_at_end() {
            if self.check(TokenType::LParen) {
                paren_depth += 1;
                let token = self.advance();
                content.push(token.value.clone());
            } else if self.check(TokenType::RParen) {
                if paren_depth == 0 {
                    // This is our closing paren, don't consume it
                    break;
                }
                paren_depth -= 1;
                let token = self.advance();
                content.push(token.value.clone());
            } else {
                let token = self.advance();
                content.push(token.value.clone());
            }
        }
        
        // Join all tokens preserving original spacing
        let preserved_text = content.join("");
        // eprintln!("parse_preserve_arguments collected: '{:?}' -> '{}'", content, preserved_text);
        vec![ExpressionNode::Text(TextNode {
            value: preserved_text,
            position: self.tokens[start_pos].position.clone(),
        })]
    }
    
    fn parse_for_arguments(&mut self) -> Vec<ExpressionNode> {
        let mut args = Vec::new();
        let start_pos = self.peek().position.clone();
        
        // Parse variable or tuple pattern
        let mut main_pattern = None;
        if self.check(TokenType::LParen) {
            // Tuple pattern
            self.advance(); // consume (
            let mut elements = Vec::new();
            
            loop {
                self.skip_whitespace();
                if self.check(TokenType::Identifier) {
                    elements.push(self.advance().value.clone());
                } else {
                    break;
                }
                
                self.skip_whitespace();
                if !self.match_token(TokenType::Comma) {
                    break;
                }
            }
            
            if !self.consume(TokenType::RParen) {
                let pos = self.peek().position.clone();
                self.add_error("Expected ')' after tuple pattern", pos);
            }
            
            let pos = self.peek().position.clone();
            main_pattern = Some(ExpressionNode::TuplePattern(TuplePatternNode {
                elements,
                position: pos,
            }));
        } else if self.check(TokenType::Identifier) {
            // Single variable
            let token = self.advance();
            main_pattern = Some(ExpressionNode::Identifier(IdentifierNode {
                name: token.value.clone(),
                position: token.position.clone(),
            }));
        }
        
        self.skip_whitespace();
        
        // Check for optional index variable (comma before 'in')
        let mut index_var = None;
        if self.check(TokenType::Comma) {
            self.advance(); // consume comma
            self.skip_whitespace();
            
            if self.check(TokenType::Identifier) {
                index_var = Some(self.advance().value.clone());
                self.skip_whitespace();
            }
        }
        
        // If we have an index variable, wrap in ForPattern
        if let Some(pattern) = main_pattern {
            if index_var.is_some() {
                args.push(ExpressionNode::ForPattern(ForPatternNode {
                    variable: Box::new(pattern),
                    index_variable: index_var,
                    position: start_pos.clone(),
                }));
            } else {
                args.push(pattern);
            }
        }
        
        // Expect 'in' keyword
        if !self.check(TokenType::In) {
            let pos = self.peek().position.clone();
            self.add_error("Expected 'in' in for loop", pos);
        } else {
            self.advance(); // consume 'in'
        }
        
        self.skip_whitespace();
        
        // Parse array expression
        if let Some(array_expr) = self.parse_expression() {
            args.push(array_expr);
        }
        
        self.skip_whitespace();
        
        // Parse body after comma
        if self.match_token(TokenType::Comma) {
            self.skip_whitespace();
            if let Some(body_expr) = self.parse_expression() {
                args.push(body_expr);
            }
        }
        
        args
    }
    
    fn parse_argument_list(&mut self) -> Vec<ExpressionNode> {
        let mut args = Vec::new();
        
        self.skip_whitespace();
        
        if !self.check(TokenType::RParen) {
            loop {
                self.skip_whitespace();
                
                if let Some(expr) = self.parse_expression() {
                    args.push(expr);
                } else {
                    break;
                }
                
                self.skip_whitespace();
                
                if !self.match_token(TokenType::Comma) {
                    break;
                }
            }
        }
        
        args
    }
    
    fn parse_expression(&mut self) -> Option<ExpressionNode> {
        // Check for array literal first
        if let Some(array_literal) = self.parse_array_literal() {
            return Some(ExpressionNode::ArrayLiteral(array_literal));
        }
        
        // Check for builtin functions
        if matches!(self.peek().token_type, TokenType::BuiltinRepeat | TokenType::BuiltinIf | TokenType::BuiltinFor | TokenType::BuiltinReverse | TokenType::BuiltinPreserve | TokenType::BuiltinLabel | TokenType::BuiltinBr | TokenType::BuiltinProc | TokenType::BuiltinLocal | TokenType::BuiltinLen | TokenType::ColonShorthand) {
            if let Some(builtin) = self.parse_builtin_function() {
                return Some(ExpressionNode::BuiltinFunction(builtin));
            }
        }
        
        // Check for { followed by builtin names (when lexer didn't combine)
        if self.check(TokenType::LBrace) {
            let saved_pos = self.current;
            self.advance(); // consume {
            if self.check(TokenType::Identifier) {
                let name = self.peek().value.clone();
                if matches!(name.as_str(), "repeat" | "if" | "for" | "reverse" | "preserve" | "proc" | "local" | "len") {
                    // Reset position for parse_builtin_function
                    self.current = saved_pos;
                    self.advance(); // consume { again
                    if let Some(builtin) = self.parse_builtin_function() {
                        return Some(ExpressionNode::BuiltinFunction(builtin));
                    }
                }
            }
            self.current = saved_pos; // reset if not a builtin
        }
        
        // Collect everything until a comma or closing paren
        let mut expressions = Vec::new();
        let start_pos = self.peek().position.clone();
        let mut paren_depth = 0;
        
        // Track raw text for simple arguments
        let mut raw_text = String::new();
        let mut is_simple_text = true;
        
        while !self.is_at_end() {
            let token = self.peek();
            
            if token.token_type == TokenType::LParen {
                if paren_depth == 0 && !expressions.is_empty() {
                    // This might be a function call, let parse_content handle it
                    if let Some(content) = self.parse_content() {
                        expressions.push(content);
                        is_simple_text = false;
                    }
                    continue;
                }
                paren_depth += 1;
                raw_text.push_str(&token.value);
                self.advance();
            } else if token.token_type == TokenType::RParen {
                if paren_depth == 0 {
                    break;
                }
                paren_depth -= 1;
                raw_text.push_str(&token.value);
                self.advance();
            } else if token.token_type == TokenType::Comma && paren_depth == 0 {
                break;
            } else if token.token_type == TokenType::RBrace && paren_depth == 0 {
                // Also break on closing brace for array literals
                break;
            } else if token.token_type == TokenType::Whitespace || token.token_type == TokenType::Newline {
                // Skip whitespace and newlines in expressions
                raw_text.push(' ');
                self.advance();
            } else if token.token_type == TokenType::Comment {
                // Skip comments entirely
                self.advance();
            } else {
                // Let parse_content handle complex content
                let before_count = expressions.len();
                if let Some(content) = self.parse_content() {
                    if let ContentNode::Text(text_node) = &content {
                        if before_count == 0 {
                            raw_text.push_str(&text_node.value);
                        } else {
                            is_simple_text = false;
                        }
                    } else {
                        is_simple_text = false;
                    }
                    expressions.push(content);
                }
            }
        }
        
        if expressions.is_empty() && raw_text.trim().is_empty() {
            return None;
        }
        
        // For simple text arguments, return as a single node
        if is_simple_text && !raw_text.trim().is_empty() {
            let trimmed = raw_text.trim();
            // Try to parse as number
            if let Ok(num) = trimmed.parse::<i64>() {
                return Some(ExpressionNode::Number(NumberNode {
                    value: num,
                    position: start_pos.clone(),
                }));
            }
            // Check for hex number
            if trimmed.starts_with("0x") || trimmed.starts_with("0X") {
                if let Ok(num) = i64::from_str_radix(&trimmed[2..], 16) {
                    return Some(ExpressionNode::Number(NumberNode {
                        value: num,
                        position: start_pos.clone(),
                    }));
                }
            }
            // Check for character literal
            if trimmed.starts_with('\'') && trimmed.ends_with('\'') && trimmed.len() == 3 {
                let ch = trimmed.chars().nth(1).unwrap();
                return Some(ExpressionNode::Number(NumberNode {
                    value: ch as i64,
                    position: start_pos.clone(),
                }));
            }
            // Otherwise return as text
            return Some(ExpressionNode::Text(TextNode {
                value: trimmed.to_string(),
                position: start_pos.clone(),
            }));
        }
        
        if expressions.len() == 1 {
            // Convert single ContentNode to ExpressionNode
            match &expressions[0] {
                ContentNode::MacroInvocation(inv) => {
                    return Some(ExpressionNode::MacroInvocation(inv.clone()));
                }
                ContentNode::BuiltinFunction(func) => {
                    return Some(ExpressionNode::BuiltinFunction(func.clone()));
                }
                ContentNode::BrainfuckCommand(cmd) => {
                    return Some(ExpressionNode::BrainfuckCommand(cmd.clone()));
                }
                ContentNode::Text(text) => {
                    // Try to parse as number or identifier
                    let trimmed = text.value.trim();
                    if let Ok(num) = trimmed.parse::<i64>() {
                        return Some(ExpressionNode::Number(NumberNode {
                            value: num,
                            position: text.position.clone(),
                        }));
                    }
                    return Some(ExpressionNode::Text(text.clone()));
                }
            }
        }
        
        // Multiple expressions, wrap in ExpressionList
        let end_pos = expressions.last()
            .map(|n| n.position().end)
            .unwrap_or(start_pos.end);
        
        Some(ExpressionNode::ExpressionList(ExpressionListNode {
            expressions,
            position: ASTPosition {
                start: start_pos.start,
                end: end_pos,
                line: start_pos.line,
                column: start_pos.column,
            },
        }))
    }
    
    fn parse_array_literal(&mut self) -> Option<ArrayLiteralNode> {
        if !self.check(TokenType::LBrace) {
            return None;
        }
        
        // Check if it's actually a builtin function (when tokens aren't combined)
        let saved_pos = self.current;
        self.advance(); // consume {
        if self.check(TokenType::Identifier) {
            let name = self.peek().value.clone();
            if matches!(name.as_str(), "repeat" | "if" | "for" | "reverse" | "proc" | "local" | "len") {
                // It's a builtin function, not an array literal
                self.current = saved_pos;
                return None;
            }
        }
        // Reset and continue as array literal
        self.current = saved_pos;
        
        let start = self.peek().position.start;
        let start_line = self.peek().position.line;
        let start_column = self.peek().position.column;
        self.advance(); // consume {
        
        let mut elements = Vec::new();
        
        self.skip_whitespace();
        
        while !self.check(TokenType::RBrace) && !self.is_at_end() {
            if let Some(expr) = self.parse_expression() {
                elements.push(expr);
            }
            
            self.skip_whitespace();
            
            if !self.match_token(TokenType::Comma) {
                break;
            }
            
            self.skip_whitespace();
        }
        
        if !self.consume(TokenType::RBrace) {
            let pos = self.peek().position.clone();
            self.add_error("Expected '}' after array literal", pos);
        }
        
        let end_pos = self.tokens[self.current.saturating_sub(1)].position.end;
        
        Some(ArrayLiteralNode {
            elements,
            position: ASTPosition {
                start,
                end: end_pos,
                line: start_line,
                column: start_column,
            },
        })
    }
    
    fn _parse_expression_list(&mut self) -> Option<ExpressionListNode> {
        let start_pos = self.peek().position.clone();
        let mut expressions = Vec::new();
        
        while !self.is_at_end() && 
              !self.check(TokenType::Comma) && 
              !self.check(TokenType::RParen) && 
              !self.check(TokenType::RBrace) &&
              !self.check(TokenType::Newline) {
            if let Some(content) = self.parse_content() {
                expressions.push(content);
            } else {
                break;
            }
        }
        
        if expressions.is_empty() {
            return None;
        }
        
        let end_pos = match expressions.last() {
            Some(node) => match node {
                ContentNode::BrainfuckCommand(n) => n.position.end,
                ContentNode::MacroInvocation(n) => n.position.end,
                ContentNode::BuiltinFunction(n) => n.position.end,
                ContentNode::Text(n) => n.position.end,
            },
            None => start_pos.end,
        };
        
        Some(ExpressionListNode {
            expressions,
            position: ASTPosition {
                start: start_pos.start,
                end: end_pos,
                line: start_pos.line,
                column: start_pos.column,
            },
        })
    }
    
    // Helper methods
    
    fn advance(&mut self) -> &Token {
        if !self.is_at_end() {
            self.current += 1;
        }
        &self.tokens[self.current - 1]
    }
    
    fn check(&self, token_type: TokenType) -> bool {
        if self.is_at_end() {
            false
        } else {
            self.peek().token_type == token_type
        }
    }
    
    fn match_token(&mut self, token_type: TokenType) -> bool {
        if self.check(token_type) {
            self.advance();
            true
        } else {
            false
        }
    }
    
    fn consume(&mut self, token_type: TokenType) -> bool {
        if self.check(token_type) {
            self.advance();
            true
        } else {
            false
        }
    }
    
    fn peek(&self) -> &Token {
        &self.tokens[self.current]
    }
    
    fn peek_ahead(&self, n: usize) -> Option<&Token> {
        let pos = self.current + n;
        if pos < self.tokens.len() {
            Some(&self.tokens[pos])
        } else {
            None
        }
    }
    
    fn is_at_end(&self) -> bool {
        self.current >= self.tokens.len() || self.peek().token_type == TokenType::Eof
    }
    
    fn skip_whitespace(&mut self) {
        while self.check(TokenType::Whitespace) {
            self.advance();
        }
    }
    
    fn skip_whitespace_and_newlines(&mut self) {
        while self.check(TokenType::Whitespace) || self.check(TokenType::Newline) ||
              self.check(TokenType::Comment) {
            self.advance();
        }
    }
    
    fn synchronize(&mut self) {
        while !self.is_at_end() {
            if self.match_token(TokenType::Newline) {
                return;
            }
            self.advance();
        }
    }
    
    fn add_error(&mut self, message: &str, position: ASTPosition) {
        self.errors.push(MacroExpansionError {
            error_type: MacroExpansionErrorType::SyntaxError,
            message: message.to_string(),
            location: Some(SourceLocation {
                line: position.line.saturating_sub(1),
                column: position.column.saturating_sub(1),
                length: position.end - position.start,
            }),
        });
    }
}