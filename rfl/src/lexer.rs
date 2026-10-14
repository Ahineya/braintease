use crate::ast::Span;
use crate::error::{CompileError, line_col};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TokenKind {
    Ident(String),
    Int(i64),
    /// Source spelling of a float literal, such as `1.5` or `1e-2`.
    Float(String),
    /// Decoded string literal. Cells are Unicode scalars that fit in 16 bits.
    Str(Vec<u16>),
    /// `c"..."`. Same cell encoding as a string, including escapes.
    CStr(Vec<u16>),
    /// A character literal. The value is a Unicode scalar.
    Char(u32),
    Fn,
    Let,
    Mut,
    If,
    Else,
    Return,
    As,
    True,
    False,
    While,
    Break,
    Continue,
    Match,
    For,
    In,
    Type,
    Impl,
    Const,
    Struct,
    Export,
    Import,
    Extern,
    Unsafe,
    Hash,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    ColonColon,
    Question,
    Dot,
    DotDot,
    DotDotEq,
    Semi,
    Arrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Eq,
    EqEq,
    FatArrow,
    Pipe,
    Not,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    Amp,
    OrOr,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    /// `<<` and `>>`.
    Shl,
    Shr,
    /// `^` and `~`.
    Caret,
    Tilde,
    /// `...` in a variadic `extern "C"` signature.
    Ellipsis,
    Eof,
}

pub fn tokenize(source: &str) -> Result<Vec<Token>, CompileError> {
    let mut lexer = Lexer { source, index: 0 };
    let mut tokens = Vec::new();
    loop {
        let token = lexer.next_token()?;
        let is_eof = token.kind == TokenKind::Eof;
        tokens.push(token);
        if is_eof {
            break;
        }
    }
    Ok(tokens)
}

struct Lexer<'a> {
    source: &'a str,
    index: usize,
}

impl<'a> Lexer<'a> {
    fn next_token(&mut self) -> Result<Token, CompileError> {
        self.skip_trivia()?;
        let start = self.index;
        let Some(ch) = self.peek_char() else {
            return Ok(Token { kind: TokenKind::Eof, span: Span { start } });
        };
        let kind = match ch {
            '(' => { self.bump(); TokenKind::LParen }
            ')' => { self.bump(); TokenKind::RParen }
            '{' => { self.bump(); TokenKind::LBrace }
            '}' => { self.bump(); TokenKind::RBrace }
            '[' => { self.bump(); TokenKind::LBracket }
            ']' => { self.bump(); TokenKind::RBracket }
            ',' => { self.bump(); TokenKind::Comma }
            ':' => {
                self.bump();
                if self.peek_char() == Some(':') {
                    self.bump();
                    TokenKind::ColonColon
                } else {
                    TokenKind::Colon
                }
            }
            '?' => { self.bump(); TokenKind::Question }
            '.' => {
                self.bump();
                if self.peek_char() == Some('.') {
                    self.bump();
                    if self.peek_char() == Some('=') {
                        self.bump();
                        TokenKind::DotDotEq
                    } else if self.peek_char() == Some('.') {
                        self.bump();
                        TokenKind::Ellipsis
                    } else {
                        TokenKind::DotDot
                    }
                } else {
                    TokenKind::Dot
                }
            }
            ';' => { self.bump(); TokenKind::Semi }
            '+' => self.op2('=', TokenKind::PlusEq, TokenKind::Plus),
            '*' => self.op2('=', TokenKind::StarEq, TokenKind::Star),
            '%' => self.op2('=', TokenKind::PercentEq, TokenKind::Percent),
            '/' => {
                self.bump();
                if self.peek_char() == Some('=') {
                    self.bump();
                    TokenKind::SlashEq
                } else {
                    TokenKind::Slash
                }
            }
            '-' => {
                self.bump();
                if self.peek_char() == Some('>') {
                    self.bump();
                    TokenKind::Arrow
                } else if self.peek_char() == Some('=') {
                    self.bump();
                    TokenKind::MinusEq
                } else {
                    TokenKind::Minus
                }
            }
            '=' => self.eq_token(),
            '!' => self.op2('=', TokenKind::NotEq, TokenKind::Not),
            '<' => self.shift_or_compare('<', TokenKind::Shl, TokenKind::Le, TokenKind::Lt),
            '>' => self.shift_or_compare('>', TokenKind::Shr, TokenKind::Ge, TokenKind::Gt),
            '^' => { self.bump(); TokenKind::Caret }
            '~' => { self.bump(); TokenKind::Tilde }
            '&' => {
                self.bump();
                if self.peek_char() == Some('&') {
                    self.bump();
                    TokenKind::AndAnd
                } else {
                    TokenKind::Amp
                }
            }
            '|' => {
                self.bump();
                if self.peek_char() == Some('|') {
                    self.bump();
                    TokenKind::OrOr
                } else {
                    TokenKind::Pipe
                }
            }
            '#' => {
                self.bump();
                TokenKind::Hash
            }
            '"' => return self.string(start, false),
            'c' if self.peek_next() == Some('"') => {
                self.bump();
                return self.string(start, true);
            }
            '\'' => self.character(start)?,
            c if c.is_ascii_digit() => self.number(start)?,
            c if is_ident_start(c) => self.ident(start),
            _ => {
                return Err(self.error(start, format!("unexpected character {ch:?}")));
            }
        };
        Ok(Token { kind, span: Span { start } })
    }

    fn number(&mut self, start: usize) -> Result<TokenKind, CompileError> {
        while self.peek_char().is_some_and(|c| c.is_ascii_digit()) {
            self.bump();
        }
        if self.peek_char() == Some('x') || self.peek_char() == Some('X') {
            return Err(self.error(start, "hex literals are not implemented yet"));
        }
        let mut is_float = false;
        if self.peek_char() == Some('.') {
            let fraction = self.source[self.index + 1..].chars().next();
            if fraction.is_some_and(|c| c.is_ascii_digit()) {
                is_float = true;
                self.bump();
                while self.peek_char().is_some_and(|c| c.is_ascii_digit()) {
                    self.bump();
                }
            }
        }
        if matches!(self.peek_char(), Some('e' | 'E')) {
            is_float = true;
            self.bump();
            if matches!(self.peek_char(), Some('+' | '-')) {
                self.bump();
            }
            if !self.peek_char().is_some_and(|c| c.is_ascii_digit()) {
                return Err(self.error(start, "a float exponent needs digits"));
            }
            while self.peek_char().is_some_and(|c| c.is_ascii_digit()) {
                self.bump();
            }
        }
        let text = &self.source[start..self.index];
        if is_float {
            if text.parse::<f64>().is_err() {
                return Err(self.error(start, format!("invalid float literal {text}")));
            }
            return Ok(TokenKind::Float(text.to_string()));
        }
        let value = text.parse::<i64>().map_err(|_| {
            self.error(start, format!("integer literal {text} does not fit in i64"))
        })?;
        Ok(TokenKind::Int(value))
    }

    fn string(&mut self, start: usize, cstr: bool) -> Result<Token, CompileError> {
        self.bump();
        let mut chars = Vec::new();
        loop {
            match self.peek_char() {
                None | Some('\n') => return Err(self.error(start, "unterminated string")),
                Some('"') => {
                    self.bump();
                    break;
                }
                Some(_) => chars.push(self.escape(start)?),
            }
        }
        let kind = if cstr { TokenKind::CStr(chars) } else { TokenKind::Str(chars) };
        Ok(Token { kind, span: Span { start } })
    }

    fn character(&mut self, start: usize) -> Result<TokenKind, CompileError> {
        self.bump();
        if self.peek_char() == Some('\'') {
            return Err(self.error(start, "empty character literal"));
        }
        let ch = self.escape(start)?;
        if self.peek_char() != Some('\'') {
            return Err(self.error(start, "character literal must be one character"));
        }
        self.bump();
        let scalar = u32::from(ch);
        if (0xD800..=0xDFFF).contains(&scalar) {
            return Err(self.error(start, "character literal is not a Unicode scalar"));
        }
        Ok(TokenKind::Char(scalar))
    }

    fn escape(&mut self, start: usize) -> Result<u16, CompileError> {
        let Some(ch) = self.peek_char() else {
            return Err(self.error(start, "unterminated literal"));
        };
        self.bump();
        if ch != '\\' {
            if (ch as u32) > 0xffff {
                return Err(self.error(start, "character does not fit in a cell"));
            }
            return Ok(ch as u16);
        }
        let Some(esc) = self.peek_char() else {
            return Err(self.error(start, "unterminated escape"));
        };
        self.bump();
        Ok(match esc {
            'n' => '\n' as u16,
            'r' => '\r' as u16,
            't' => '\t' as u16,
            '0' => 0,
            '\\' => '\\' as u16,
            '\'' => '\'' as u16,
            '"' => '"' as u16,
            _ => return Err(self.error(start, format!("unknown escape '\\{esc}'"))),
        })
    }

    fn ident(&mut self, start: usize) -> TokenKind {
        self.bump();
        while self.peek_char().is_some_and(is_ident_continue) {
            self.bump();
        }
        let text = &self.source[start..self.index];
        match text {
            "fn" => TokenKind::Fn,
            "let" => TokenKind::Let,
            "mut" => TokenKind::Mut,
            "if" => TokenKind::If,
            "else" => TokenKind::Else,
            "return" => TokenKind::Return,
            "as" => TokenKind::As,
            "true" => TokenKind::True,
            "false" => TokenKind::False,
            "while" => TokenKind::While,
            "break" => TokenKind::Break,
            "continue" => TokenKind::Continue,
            "match" => TokenKind::Match,
            "for" => TokenKind::For,
            "in" => TokenKind::In,
            "type" => TokenKind::Type,
            "impl" => TokenKind::Impl,
            "const" => TokenKind::Const,
            "struct" => TokenKind::Struct,
            "export" => TokenKind::Export,
            "import" => TokenKind::Import,
            "extern" => TokenKind::Extern,
            "unsafe" => TokenKind::Unsafe,
            _ => TokenKind::Ident(text.to_string()),
        }
    }

    fn eq_token(&mut self) -> TokenKind {
        self.bump();
        if self.peek_char() == Some('=') {
            self.bump();
            TokenKind::EqEq
        } else if self.peek_char() == Some('>') {
            self.bump();
            TokenKind::FatArrow
        } else {
            TokenKind::Eq
        }
    }

    fn shift_or_compare(&mut self, again: char, shift: TokenKind, equal: TokenKind, one: TokenKind) -> TokenKind {
        self.bump();
        if self.peek_char() == Some(again) {
            self.bump();
            shift
        } else if self.peek_char() == Some('=') {
            self.bump();
            equal
        } else {
            one
        }
    }

    fn op2(&mut self, second: char, yes: TokenKind, no: TokenKind) -> TokenKind {
        self.bump();
        if self.peek_char() == Some(second) {
            self.bump();
            yes
        } else {
            no
        }
    }

    fn skip_trivia(&mut self) -> Result<(), CompileError> {
        loop {
            while self.peek_char().is_some_and(|c| c.is_whitespace()) {
                self.bump();
            }
            if self.peek_char() == Some('/') && self.peek_next() == Some('/') {
                while self.peek_char().is_some_and(|c| c != '\n') {
                    self.bump();
                }
                continue;
            }
            if self.peek_char() == Some('/') && self.peek_next() == Some('*') {
                let start = self.index;
                self.bump();
                self.bump();
                loop {
                    match self.peek_char() {
                        None => return Err(self.error(start, "unterminated block comment")),
                        Some('*') if self.peek_next() == Some('/') => {
                            self.bump();
                            self.bump();
                            break;
                        }
                        Some(_) => self.bump(),
                    }
                }
                continue;
            }
            break;
        }
        Ok(())
    }

    fn peek_char(&self) -> Option<char> {
        self.source[self.index..].chars().next()
    }

    fn peek_next(&self) -> Option<char> {
        let mut chars = self.source[self.index..].chars();
        chars.next()?;
        chars.next()
    }

    fn bump(&mut self) {
        if let Some(ch) = self.peek_char() {
            self.index += ch.len_utf8();
        }
    }

    fn error(&self, offset: usize, message: impl Into<String>) -> CompileError {
        let (line, column) = line_col(self.source, offset);
        CompileError {
            message: format!("{line}:{column}: {}", message.into()),
        }
    }
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_continue(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}
