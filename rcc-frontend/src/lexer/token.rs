//! Token definitions for the C99 lexer
//! 
//! This module defines token types and the Token struct.

use rcc_common::{SourceLocation, SourceSpan};
use serde::{Deserialize, Serialize};
use std::fmt;

/// C99 Token types
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum TokenType {
    // Literals
    IntLiteral(i64),
    CharLiteral(u8),
    StringLiteral(String),
    
    // Identifiers and keywords
    Identifier(String),
    
    // Keywords
    Auto, Break, Case, Char, Const, Continue, Default, Do,
    Double, Else, Enum, Extern, Float, For, Goto, If,
    Int, Long, Register, Return, Short, Signed, Sizeof, Static,
    Struct, Switch, Typedef, Union, Unsigned, Void, Volatile, While,
    Asm,  // For inline assembly
    
    // Operators
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %
    Ampersand,      // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    Bang,           // !
    Equal,          // =
    Less,           // <
    Greater,        // >
    Question,       // ?
    Colon,          // :
    
    // Compound operators
    PlusPlus,       // ++
    MinusMinus,     // --
    LeftShift,      // <<
    RightShift,     // >>
    LessEqual,      // <=
    GreaterEqual,   // >=
    EqualEqual,     // ==
    BangEqual,      // !=
    AmpersandAmpersand, // &&
    PipePipe,       // ||
    
    // Assignment operators
    PlusEqual,      // +=
    MinusEqual,     // -=
    StarEqual,      // *=
    SlashEqual,     // /=
    PercentEqual,   // %=
    AmpersandEqual, // &=
    PipeEqual,      // |=
    CaretEqual,     // ^=
    LeftShiftEqual, // <<=
    RightShiftEqual, // >>=
    
    // Delimiters
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Semicolon,      // ;
    Comma,          // ,
    Dot,            // .
    Arrow,          // ->
    
    // Special
    Newline,
    EndOfFile,
    
    // Comments (optional - may be stripped)
    LineComment(String),
    BlockComment(String),
}

impl fmt::Display for TokenType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TokenType::IntLiteral(n) => write!(f, "{n}"),
            TokenType::CharLiteral(c) => write!(f, "'{}'", *c as char),
            TokenType::StringLiteral(s) => write!(f, "\"{s}\""),
            TokenType::Identifier(s) => write!(f, "{s}"),
            
            // Keywords
            TokenType::Auto => write!(f, "auto"),
            TokenType::Break => write!(f, "break"),
            TokenType::Case => write!(f, "case"),
            TokenType::Char => write!(f, "char"),
            TokenType::Const => write!(f, "const"),
            TokenType::Continue => write!(f, "continue"),
            TokenType::Default => write!(f, "default"),
            TokenType::Do => write!(f, "do"),
            TokenType::Double => write!(f, "double"),
            TokenType::Else => write!(f, "else"),
            TokenType::Enum => write!(f, "enum"),
            TokenType::Extern => write!(f, "extern"),
            TokenType::Float => write!(f, "float"),
            TokenType::For => write!(f, "for"),
            TokenType::Goto => write!(f, "goto"),
            TokenType::If => write!(f, "if"),
            TokenType::Int => write!(f, "int"),
            TokenType::Long => write!(f, "long"),
            TokenType::Register => write!(f, "register"),
            TokenType::Return => write!(f, "return"),
            TokenType::Short => write!(f, "short"),
            TokenType::Signed => write!(f, "signed"),
            TokenType::Sizeof => write!(f, "sizeof"),
            TokenType::Static => write!(f, "static"),
            TokenType::Struct => write!(f, "struct"),
            TokenType::Switch => write!(f, "switch"),
            TokenType::Typedef => write!(f, "typedef"),
            TokenType::Union => write!(f, "union"),
            TokenType::Unsigned => write!(f, "unsigned"),
            TokenType::Void => write!(f, "void"),
            TokenType::Volatile => write!(f, "volatile"),
            TokenType::While => write!(f, "while"),
            TokenType::Asm => write!(f, "asm"),
            
            // Operators - show the symbol
            TokenType::Plus => write!(f, "+"),
            TokenType::Minus => write!(f, "-"),
            TokenType::Star => write!(f, "*"),
            TokenType::Slash => write!(f, "/"),
            TokenType::Percent => write!(f, "%"),
            TokenType::Ampersand => write!(f, "&"),
            TokenType::Pipe => write!(f, "|"),
            TokenType::Caret => write!(f, "^"),
            TokenType::Tilde => write!(f, "~"),
            TokenType::Bang => write!(f, "!"),
            TokenType::Equal => write!(f, "="),
            TokenType::Less => write!(f, "<"),
            TokenType::Greater => write!(f, ">"),
            TokenType::Question => write!(f, "?"),
            TokenType::Colon => write!(f, ":"),
            
            TokenType::PlusPlus => write!(f, "++"),
            TokenType::MinusMinus => write!(f, "--"),
            TokenType::LeftShift => write!(f, "<<"),
            TokenType::RightShift => write!(f, ">>"),
            TokenType::LessEqual => write!(f, "<="),
            TokenType::GreaterEqual => write!(f, ">="),
            TokenType::EqualEqual => write!(f, "=="),
            TokenType::BangEqual => write!(f, "!="),
            TokenType::AmpersandAmpersand => write!(f, "&&"),
            TokenType::PipePipe => write!(f, "||"),
            
            TokenType::PlusEqual => write!(f, "+="),
            TokenType::MinusEqual => write!(f, "-="),
            TokenType::StarEqual => write!(f, "*="),
            TokenType::SlashEqual => write!(f, "/="),
            TokenType::PercentEqual => write!(f, "%="),
            TokenType::AmpersandEqual => write!(f, "&="),
            TokenType::PipeEqual => write!(f, "|="),
            TokenType::CaretEqual => write!(f, "^="),
            TokenType::LeftShiftEqual => write!(f, "<<="),
            TokenType::RightShiftEqual => write!(f, ">>="),
            
            TokenType::LeftParen => write!(f, "("),
            TokenType::RightParen => write!(f, ")"),
            TokenType::LeftBrace => write!(f, "{{"),
            TokenType::RightBrace => write!(f, "}}"),
            TokenType::LeftBracket => write!(f, "["),
            TokenType::RightBracket => write!(f, "]"),
            TokenType::Semicolon => write!(f, ";"),
            TokenType::Comma => write!(f, ","),
            TokenType::Dot => write!(f, "."),
            TokenType::Arrow => write!(f, "->"),
            
            TokenType::Newline => write!(f, "\\n"),
            TokenType::EndOfFile => write!(f, "EOF"),
            TokenType::LineComment(s) => write!(f, "//{s}"),
            TokenType::BlockComment(s) => write!(f, "/*{s}*/"),
        }
    }
}

/// A token with location information
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Token {
    pub token_type: TokenType,
    pub span: SourceSpan,
}

impl Token {
    pub fn new(token_type: TokenType, span: SourceSpan) -> Self {
        Self { token_type, span }
    }
    
    pub fn eof(location: SourceLocation) -> Self {
        Self {
            token_type: TokenType::EndOfFile,
            span: SourceSpan::new(location.clone(), location),
        }
    }
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at {}", self.token_type, self.span.start)
    }
}