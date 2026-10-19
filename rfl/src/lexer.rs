use crate::ast::Span;
use crate::error::CompileError;

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
    /// Decoded string literal in UTF-16 code units; checked strings become UTF-8.
    Str(Vec<u16>),
    /// `c"..."`. One C16 cell per BMP scalar; supplementary scalars are rejected.
    CStr(Vec<u16>),
    /// A character literal. The value is a Unicode scalar.
    Char(u32),
    Fn,
    Async,
    Await,
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
    Trait,
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
    /// `// ...` through the end of the line. The newline stays outside the token.
    LineComment,
    /// `/* ... */`, including the delimiters.
    BlockComment,
    MacroBody(crate::macros::syntax::RawGroup),
    Eof,
}

pub fn tokenize(source: &str) -> Result<Vec<Token>, CompileError> {
    let tokens = tokenize_with_trivia(source)?;
    Ok(tokens
        .into_iter()
        .filter(|token| !matches!(token.kind, TokenKind::LineComment | TokenKind::BlockComment))
        .collect())
}

/// Every token, including comments. The parser uses [`tokenize`], which drops comments.
pub fn tokenize_with_trivia(source: &str) -> Result<Vec<Token>, CompileError> {
    let mut lexer = Lexer {
        source,
        index: 0,
        trivia: true,
    };
    let mut tokens = Vec::new();
    loop {
        let mut token = lexer.next_token()?;
        if matches!(
            token.kind,
            TokenKind::LBrace | TokenKind::LParen | TokenKind::LBracket
        ) {
            let mut previous = tokens.iter().rev().filter(|t: &&Token| {
                !matches!(t.kind, TokenKind::LineComment | TokenKind::BlockComment)
            });
            if previous.next().is_some_and(|t| t.kind == TokenKind::Not)
                && previous
                    .next()
                    .is_some_and(|t| matches!(t.kind, TokenKind::Ident(_)))
            {
                let (group, end) = crate::macros::syntax::capture(source, token.span.start)?;
                lexer.index = end;
                token.span.end = end;
                token.kind = TokenKind::MacroBody(group);
            }
        }
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
    trivia: bool,
}

impl<'a> Lexer<'a> {
    fn next_token(&mut self) -> Result<Token, CompileError> {
        if let Some(comment) = self.skip_trivia()? {
            return Ok(comment);
        }
        let start = self.index;
        let Some(ch) = self.peek_char() else {
            return Ok(Token {
                kind: TokenKind::Eof,
                span: Span::new(start, start),
            });
        };
        let kind = match ch {
            '(' => {
                self.bump();
                TokenKind::LParen
            }
            ')' => {
                self.bump();
                TokenKind::RParen
            }
            '{' => {
                self.bump();
                TokenKind::LBrace
            }
            '}' => {
                self.bump();
                TokenKind::RBrace
            }
            '[' => {
                self.bump();
                TokenKind::LBracket
            }
            ']' => {
                self.bump();
                TokenKind::RBracket
            }
            ',' => {
                self.bump();
                TokenKind::Comma
            }
            ':' => {
                self.bump();
                if self.peek_char() == Some(':') {
                    self.bump();
                    TokenKind::ColonColon
                } else {
                    TokenKind::Colon
                }
            }
            '?' => {
                self.bump();
                TokenKind::Question
            }
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
            ';' => {
                self.bump();
                TokenKind::Semi
            }
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
            '^' => {
                self.bump();
                TokenKind::Caret
            }
            '~' => {
                self.bump();
                TokenKind::Tilde
            }
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
        Ok(Token {
            kind,
            span: Span::new(start, self.index),
        })
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
                Some(_) => {
                    let scalar = self.escape(start)?;
                    if cstr {
                        chars.push(u16::try_from(scalar).map_err(|_| {
                            self.error(start, "C string character does not fit in a cell")
                        })?);
                    } else {
                        let ch = char::from_u32(scalar).ok_or_else(|| {
                            self.error(start, "string contains an invalid Unicode scalar")
                        })?;
                        let mut units = [0u16; 2];
                        chars.extend_from_slice(ch.encode_utf16(&mut units));
                    }
                }
            }
        }
        let kind = if cstr {
            TokenKind::CStr(chars)
        } else {
            TokenKind::Str(chars)
        };
        Ok(Token {
            kind,
            span: Span::new(start, self.index),
        })
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
        let scalar = ch;
        if (0xD800..=0xDFFF).contains(&scalar) {
            return Err(self.error(start, "character literal is not a Unicode scalar"));
        }
        Ok(TokenKind::Char(scalar))
    }

    fn escape(&mut self, start: usize) -> Result<u32, CompileError> {
        let Some(ch) = self.peek_char() else {
            return Err(self.error(start, "unterminated literal"));
        };
        self.bump();
        if ch != '\\' {
            return Ok(ch as u32);
        }
        let Some(esc) = self.peek_char() else {
            return Err(self.error(start, "unterminated escape"));
        };
        self.bump();
        Ok(match esc {
            'n' => '\n' as u32,
            'r' => '\r' as u32,
            't' => '\t' as u32,
            '0' => 0,
            '\\' => '\\' as u32,
            '\'' => '\'' as u32,
            '"' => '"' as u32,
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
            "async" => TokenKind::Async,
            "await" => TokenKind::Await,
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
            "trait" => TokenKind::Trait,
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

    fn shift_or_compare(
        &mut self,
        again: char,
        shift: TokenKind,
        equal: TokenKind,
        one: TokenKind,
    ) -> TokenKind {
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

    fn skip_trivia(&mut self) -> Result<Option<Token>, CompileError> {
        loop {
            while self.peek_char().is_some_and(|c| c.is_whitespace()) {
                self.bump();
            }
            if self.peek_char() == Some('/') && self.peek_next() == Some('/') {
                let start = self.index;
                while self.peek_char().is_some_and(|c| c != '\n') {
                    self.bump();
                }
                if self.trivia {
                    return Ok(Some(Token {
                        kind: TokenKind::LineComment,
                        span: Span::new(start, self.index),
                    }));
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
                if self.trivia {
                    return Ok(Some(Token {
                        kind: TokenKind::BlockComment,
                        span: Span::new(start, self.index),
                    }));
                }
                continue;
            }
            break;
        }
        Ok(None)
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
        CompileError::at(self.source, offset, message)
    }
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_continue(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}
