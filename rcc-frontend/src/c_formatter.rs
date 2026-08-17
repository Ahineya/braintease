//! C syntax highlighting formatter for TUI display
//! 
//! This module provides functions to format C code with colored spans
//! for display in ratatui-based terminal interfaces.

use ratatui::prelude::{Color, Modifier, Span, Style};
use crate::lexer::{Lexer, TokenType};

/// Formats a line of C code with syntax highlighting
pub fn format_c_line(line: &str) -> Vec<Span<'static>> {
    let mut lexer = Lexer::new(line);
    let tokens = match lexer.tokenize() {
        Ok(tokens) => tokens,
        Err(_) => {
            // If lexing fails, return the line as-is
            return vec![Span::raw(line.to_string())];
        }
    };
    
    let mut spans = Vec::new();
    let mut last_pos = 0usize;
    let line_bytes = line.as_bytes();
    
    for token in tokens {
        // Add any text between tokens (whitespace, etc.)
        let token_start = token.span.start.column.saturating_sub(1) as usize;
        if token_start > last_pos && last_pos < line.len() {
            let between = std::str::from_utf8(&line_bytes[last_pos..token_start.min(line.len())])
                .unwrap_or("")
                .to_string();
            spans.push(Span::raw(between));
        }
        
        // Style the token based on its type
        let token_text = {
            let start = token.span.start.column.saturating_sub(1) as usize;
            let end = token.span.end.column.saturating_sub(1) as usize;
            if start < line.len() && end <= line.len() && start < end {
                std::str::from_utf8(&line_bytes[start..end])
                    .unwrap_or("")
                    .to_string()
            } else {
                // Fallback to token display
                token.token_type.to_string()
            }
        };
        
        let styled_span = match &token.token_type {
            // Keywords - cyan
            TokenType::Auto | TokenType::Break | TokenType::Case | TokenType::Char |
            TokenType::Const | TokenType::Continue | TokenType::Default | TokenType::Do |
            TokenType::Double | TokenType::Else | TokenType::Enum | TokenType::Extern |
            TokenType::Float | TokenType::For | TokenType::Goto | TokenType::If |
            TokenType::Int | TokenType::Long | TokenType::Register | TokenType::Return |
            TokenType::Short | TokenType::Signed | TokenType::Sizeof | TokenType::Static |
            TokenType::Struct | TokenType::Switch | TokenType::Typedef | TokenType::Union |
            TokenType::Unsigned | TokenType::Void | TokenType::Volatile | TokenType::While |
            TokenType::Bool | TokenType::Asm => {
                Span::styled(token_text, Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            }
            
            // Literals - different colors
            TokenType::IntLiteral(_) => {
                Span::styled(token_text, Style::default().fg(Color::Rgb(255, 140, 0))) // Orange
            }
            TokenType::CharLiteral(_) => {
                Span::styled(token_text, Style::default().fg(Color::Rgb(255, 100, 100))) // Light red
            }
            TokenType::StringLiteral(_) => {
                Span::styled(token_text, Style::default().fg(Color::Rgb(0, 200, 0))) // Green
            }
            
            // Identifiers - default or special cases
            TokenType::Identifier(name) => {
                // Common C library functions - yellow
                if is_common_c_function(name) {
                    Span::styled(token_text, Style::default().fg(Color::Yellow))
                } else {
                    Span::styled(token_text, Style::default().fg(Color::White))
                }
            }
            
            // Operators - blue
            TokenType::Plus | TokenType::Minus | TokenType::Star | TokenType::Slash |
            TokenType::Percent | TokenType::Ampersand | TokenType::Pipe | TokenType::Caret |
            TokenType::Tilde | TokenType::Bang | TokenType::Equal | TokenType::Less |
            TokenType::Greater | TokenType::Question | TokenType::Colon |
            TokenType::PlusPlus | TokenType::MinusMinus | TokenType::LeftShift |
            TokenType::RightShift | TokenType::LessEqual | TokenType::GreaterEqual |
            TokenType::EqualEqual | TokenType::BangEqual | TokenType::AmpersandAmpersand |
            TokenType::PipePipe | TokenType::PlusEqual | TokenType::MinusEqual |
            TokenType::StarEqual | TokenType::SlashEqual | TokenType::PercentEqual |
            TokenType::AmpersandEqual | TokenType::PipeEqual | TokenType::CaretEqual |
            TokenType::LeftShiftEqual | TokenType::RightShiftEqual | TokenType::Arrow |
            TokenType::Ellipsis => {
                Span::styled(token_text, Style::default().fg(Color::Blue))
            }
            
            // Delimiters - gray
            TokenType::LeftParen | TokenType::RightParen | TokenType::LeftBrace |
            TokenType::RightBrace | TokenType::LeftBracket | TokenType::RightBracket |
            TokenType::Semicolon | TokenType::Comma | TokenType::Dot => {
                Span::styled(token_text, Style::default().fg(Color::Gray))
            }
            
            // Comments - dark gray
            TokenType::LineComment(_) | TokenType::BlockComment(_) => {
                Span::styled(token_text, Style::default().fg(Color::DarkGray))
            }
            
            // Special tokens
            TokenType::Newline | TokenType::EndOfFile => {
                // Don't render these
                continue;
            }
        };
        
        spans.push(styled_span);
        last_pos = token.span.end.column.saturating_sub(1) as usize;
    }
    
    // Add any remaining text after the last token
    if last_pos < line.len() {
        let remaining = std::str::from_utf8(&line_bytes[last_pos..])
            .unwrap_or("")
            .to_string();
        spans.push(Span::raw(remaining));
    }
    
    spans
}

/// Check if an identifier is a common C library function
fn is_common_c_function(name: &str) -> bool {
    matches!(name, 
        "printf" | "scanf" | "putchar" | "getchar" | "puts" | "gets" |
        "malloc" | "calloc" | "realloc" | "free" |
        "memcpy" | "memmove" | "memset" | "memcmp" |
        "strcpy" | "strncpy" | "strcat" | "strncat" | "strcmp" | "strncmp" |
        "strlen" | "strchr" | "strrchr" | "strstr" |
        "fopen" | "fclose" | "fread" | "fwrite" | "fprintf" | "fscanf" |
        "fgets" | "fputs" | "fgetc" | "fputc" |
        "exit" | "abort" | "atoi" | "atof" | "atol" |
        "rand" | "srand" | "abs" | "labs" |
        "sin" | "cos" | "tan" | "sqrt" | "pow" | "exp" | "log"
    )
}

/// Formats C code with line numbers and syntax highlighting
pub fn format_c_code_with_line_numbers(code: &str) -> Vec<Vec<Span<'static>>> {
    code.lines()
        .enumerate()
        .map(|(i, line)| {
            let mut spans = vec![
                // Line number in dark gray
                Span::styled(
                    format!("{:4} | ", i + 1),
                    Style::default().fg(Color::DarkGray)
                ),
            ];
            
            // Add the highlighted line
            spans.extend(format_c_line(line));
            spans
        })
        .collect()
}