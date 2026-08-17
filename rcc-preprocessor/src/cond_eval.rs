//! C99 `#if` / `#elif` integer constant expression evaluator.

use anyhow::{anyhow, Result};

#[derive(Debug, Clone, PartialEq)]
enum Tok {
    Number(i64),
    Ident(String),
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Lt,
    Gt,
    Le,
    Ge,
    Eq,
    Ne,
    AndAnd,
    OrOr,
    Not,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,
    LParen,
    RParen,
    Question,
    Colon,
    Eof,
}

struct Lexer<'a> {
    chars: &'a [char],
    pos: usize,
}

impl<'a> Lexer<'a> {
    fn new(input: &'a [char]) -> Self {
        Self { chars: input, pos: 0 }
    }

    fn peek(&self) -> char {
        self.chars.get(self.pos).copied().unwrap_or('\0')
    }

    fn peek_at(&self, n: usize) -> char {
        self.chars.get(self.pos + n).copied().unwrap_or('\0')
    }

    fn bump(&mut self) -> char {
        let ch = self.peek();
        if self.pos < self.chars.len() {
            self.pos += 1;
        }
        ch
    }

    fn skip_ws(&mut self) {
        while self.peek().is_whitespace() {
            self.bump();
        }
    }

    fn next_token(&mut self) -> Result<Tok> {
        self.skip_ws();
        let ch = self.peek();
        if ch == '\0' {
            return Ok(Tok::Eof);
        }

        if ch == '\'' {
            return self.lex_char();
        }

        if ch.is_ascii_digit() {
            return self.lex_number();
        }

        if ch.is_ascii_alphabetic() || ch == '_' {
            let mut ident = String::new();
            while self.peek().is_ascii_alphanumeric() || self.peek() == '_' {
                ident.push(self.bump());
            }
            return Ok(Tok::Ident(ident));
        }

        let tok = match ch {
            '+' => {
                self.bump();
                Tok::Plus
            }
            '-' => {
                self.bump();
                Tok::Minus
            }
            '*' => {
                self.bump();
                Tok::Star
            }
            '/' => {
                self.bump();
                Tok::Slash
            }
            '%' => {
                self.bump();
                Tok::Percent
            }
            '~' => {
                self.bump();
                Tok::BitNot
            }
            '?' => {
                self.bump();
                Tok::Question
            }
            ':' => {
                self.bump();
                Tok::Colon
            }
            '(' => {
                self.bump();
                Tok::LParen
            }
            ')' => {
                self.bump();
                Tok::RParen
            }
            '!' => {
                self.bump();
                if self.peek() == '=' {
                    self.bump();
                    Tok::Ne
                } else {
                    Tok::Not
                }
            }
            '=' => {
                self.bump();
                if self.peek() == '=' {
                    self.bump();
                    Tok::Eq
                } else {
                    return Err(anyhow!("Unexpected '=' in #if expression"));
                }
            }
            '&' => {
                self.bump();
                if self.peek() == '&' {
                    self.bump();
                    Tok::AndAnd
                } else {
                    Tok::BitAnd
                }
            }
            '|' => {
                self.bump();
                if self.peek() == '|' {
                    self.bump();
                    Tok::OrOr
                } else {
                    Tok::BitOr
                }
            }
            '^' => {
                self.bump();
                Tok::BitXor
            }
            '<' => {
                self.bump();
                if self.peek() == '<' {
                    self.bump();
                    Tok::Shl
                } else if self.peek() == '=' {
                    self.bump();
                    Tok::Le
                } else {
                    Tok::Lt
                }
            }
            '>' => {
                self.bump();
                if self.peek() == '>' {
                    self.bump();
                    Tok::Shr
                } else if self.peek() == '=' {
                    self.bump();
                    Tok::Ge
                } else {
                    Tok::Gt
                }
            }
            other => {
                return Err(anyhow!("Unexpected character '{other}' in #if expression"));
            }
        };
        Ok(tok)
    }

    fn lex_number(&mut self) -> Result<Tok> {
        let mut s = String::new();
        if self.peek() == '0' && (self.peek_at(1) == 'x' || self.peek_at(1) == 'X') {
            s.push(self.bump());
            s.push(self.bump());
            while self.peek().is_ascii_hexdigit() {
                s.push(self.bump());
            }
            self.skip_number_suffix();
            let digits = &s[2..];
            let value = i64::from_str_radix(digits, 16)
                .map_err(|_| anyhow!("Invalid hex constant in #if: {s}"))?;
            return Ok(Tok::Number(value));
        }

        while self.peek().is_ascii_digit() {
            s.push(self.bump());
        }
        self.skip_number_suffix();

        let value = if s.starts_with('0') && s.len() > 1 {
            i64::from_str_radix(&s, 8).map_err(|_| anyhow!("Invalid octal constant in #if: {s}"))?
        } else {
            s.parse::<i64>().map_err(|_| anyhow!("Invalid integer constant in #if: {s}"))?
        };
        Ok(Tok::Number(value))
    }

    fn skip_number_suffix(&mut self) {
        while matches!(self.peek(), 'u' | 'U' | 'l' | 'L') {
            self.bump();
        }
    }

    fn lex_char(&mut self) -> Result<Tok> {
        self.bump(); // opening quote
        if self.peek() == '\0' {
            return Err(anyhow!("Unterminated character constant in #if"));
        }
        let value = if self.peek() == '\\' {
            self.bump();
            match self.bump() {
                'n' => b'\n' as i64,
                't' => b'\t' as i64,
                'r' => b'\r' as i64,
                '0' => 0,
                '\\' => b'\\' as i64,
                '\'' => b'\'' as i64,
                '"' => b'"' as i64,
                other => other as i64,
            }
        } else {
            self.bump() as i64
        };
        if self.peek() != '\'' {
            return Err(anyhow!("Unterminated character constant in #if"));
        }
        self.bump();
        Ok(Tok::Number(value))
    }
}

struct Parser {
    tokens: Vec<Tok>,
    pos: usize,
}

impl Parser {
    fn peek(&self) -> &Tok {
        self.tokens.get(self.pos).unwrap_or(&Tok::Eof)
    }

    fn bump(&mut self) -> Tok {
        if self.pos < self.tokens.len() {
            let tok = self.tokens[self.pos].clone();
            self.pos += 1;
            tok
        } else {
            Tok::Eof
        }
    }

    fn expect(&mut self, expected: Tok, what: &str) -> Result<()> {
        if std::mem::discriminant(self.peek()) == std::mem::discriminant(&expected) {
            self.bump();
            Ok(())
        } else {
            Err(anyhow!("Expected {what} in #if expression, got {:?}", self.peek()))
        }
    }

    fn parse_cond(&mut self) -> Result<i64> {
        let value = self.parse_lor()?;
        if matches!(self.peek(), Tok::Question) {
            self.bump();
            let then_v = self.parse_cond()?;
            self.expect(Tok::Colon, "':'")?;
            let else_v = self.parse_cond()?;
            Ok(if value != 0 { then_v } else { else_v })
        } else {
            Ok(value)
        }
    }

    fn parse_lor(&mut self) -> Result<i64> {
        let mut v = self.parse_land()?;
        while matches!(self.peek(), Tok::OrOr) {
            self.bump();
            let r = self.parse_land()?;
            v = i64::from(v != 0 || r != 0);
        }
        Ok(v)
    }

    fn parse_land(&mut self) -> Result<i64> {
        let mut v = self.parse_bitor()?;
        while matches!(self.peek(), Tok::AndAnd) {
            self.bump();
            let r = self.parse_bitor()?;
            v = i64::from(v != 0 && r != 0);
        }
        Ok(v)
    }

    fn parse_bitor(&mut self) -> Result<i64> {
        let mut v = self.parse_bitxor()?;
        while matches!(self.peek(), Tok::BitOr) {
            self.bump();
            v |= self.parse_bitxor()?;
        }
        Ok(v)
    }

    fn parse_bitxor(&mut self) -> Result<i64> {
        let mut v = self.parse_bitand()?;
        while matches!(self.peek(), Tok::BitXor) {
            self.bump();
            v ^= self.parse_bitand()?;
        }
        Ok(v)
    }

    fn parse_bitand(&mut self) -> Result<i64> {
        let mut v = self.parse_eq()?;
        while matches!(self.peek(), Tok::BitAnd) {
            self.bump();
            v &= self.parse_eq()?;
        }
        Ok(v)
    }

    fn parse_eq(&mut self) -> Result<i64> {
        let mut v = self.parse_rel()?;
        loop {
            match self.peek() {
                Tok::Eq => {
                    self.bump();
                    v = i64::from(v == self.parse_rel()?);
                }
                Tok::Ne => {
                    self.bump();
                    v = i64::from(v != self.parse_rel()?);
                }
                _ => break,
            }
        }
        Ok(v)
    }

    fn parse_rel(&mut self) -> Result<i64> {
        let mut v = self.parse_shift()?;
        loop {
            match self.peek() {
                Tok::Lt => {
                    self.bump();
                    v = i64::from(v < self.parse_shift()?);
                }
                Tok::Gt => {
                    self.bump();
                    v = i64::from(v > self.parse_shift()?);
                }
                Tok::Le => {
                    self.bump();
                    v = i64::from(v <= self.parse_shift()?);
                }
                Tok::Ge => {
                    self.bump();
                    v = i64::from(v >= self.parse_shift()?);
                }
                _ => break,
            }
        }
        Ok(v)
    }

    fn parse_shift(&mut self) -> Result<i64> {
        let mut v = self.parse_add()?;
        loop {
            match self.peek() {
                Tok::Shl => {
                    self.bump();
                    v = v.wrapping_shl((self.parse_add()? as u32) & 63);
                }
                Tok::Shr => {
                    self.bump();
                    v = v.wrapping_shr((self.parse_add()? as u32) & 63);
                }
                _ => break,
            }
        }
        Ok(v)
    }

    fn parse_add(&mut self) -> Result<i64> {
        let mut v = self.parse_mul()?;
        loop {
            match self.peek() {
                Tok::Plus => {
                    self.bump();
                    v = v.wrapping_add(self.parse_mul()?);
                }
                Tok::Minus => {
                    self.bump();
                    v = v.wrapping_sub(self.parse_mul()?);
                }
                _ => break,
            }
        }
        Ok(v)
    }

    fn parse_mul(&mut self) -> Result<i64> {
        let mut v = self.parse_unary()?;
        loop {
            match self.peek() {
                Tok::Star => {
                    self.bump();
                    v = v.wrapping_mul(self.parse_unary()?);
                }
                Tok::Slash => {
                    self.bump();
                    let r = self.parse_unary()?;
                    if r == 0 {
                        return Err(anyhow!("Division by zero in #if expression"));
                    }
                    v /= r;
                }
                Tok::Percent => {
                    self.bump();
                    let r = self.parse_unary()?;
                    if r == 0 {
                        return Err(anyhow!("Modulo by zero in #if expression"));
                    }
                    v %= r;
                }
                _ => break,
            }
        }
        Ok(v)
    }

    fn parse_unary(&mut self) -> Result<i64> {
        match self.peek() {
            Tok::Plus => {
                self.bump();
                self.parse_unary()
            }
            Tok::Minus => {
                self.bump();
                Ok(self.parse_unary()?.wrapping_neg())
            }
            Tok::Not => {
                self.bump();
                Ok(i64::from(self.parse_unary()? == 0))
            }
            Tok::BitNot => {
                self.bump();
                Ok(!self.parse_unary()?)
            }
            _ => self.parse_primary(),
        }
    }

    fn parse_primary(&mut self) -> Result<i64> {
        match self.bump() {
            Tok::Number(n) => Ok(n),
            Tok::Ident(_) => Ok(0),
            Tok::LParen => {
                let v = self.parse_cond()?;
                self.expect(Tok::RParen, "')'")?;
                Ok(v)
            }
            Tok::Eof => Err(anyhow!("Unexpected end of #if expression")),
            other => Err(anyhow!("Unexpected token {other:?} in #if expression")),
        }
    }
}

/// Evaluate a fully macro-expanded `#if` expression. Remaining identifiers are 0.
pub fn eval_expanded(expr: &str) -> Result<i64> {
    let chars: Vec<char> = expr.chars().collect();
    let mut lexer = Lexer::new(&chars);
    let mut tokens = Vec::new();
    loop {
        let tok = lexer.next_token()?;
        let is_eof = matches!(tok, Tok::Eof);
        tokens.push(tok);
        if is_eof {
            break;
        }
    }
    let mut parser = Parser { tokens, pos: 0 };
    let value = parser.parse_cond()?;
    if !matches!(parser.peek(), Tok::Eof) {
        return Err(anyhow!(
            "Trailing tokens in #if expression starting at {:?}",
            parser.peek()
        ));
    }
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::eval_expanded;

    #[test]
    fn arithmetic() {
        assert_eq!(eval_expanded("1+1").unwrap(), 2);
        assert_eq!(eval_expanded("2*3+4").unwrap(), 10);
        assert_eq!(eval_expanded("10/3").unwrap(), 3);
        assert_eq!(eval_expanded("1 && 0").unwrap(), 0);
        assert_eq!(eval_expanded("1 || 0").unwrap(), 1);
        assert_eq!(eval_expanded("2 == 2").unwrap(), 1);
        assert_eq!(eval_expanded("VERSION").unwrap(), 0);
        assert_eq!(eval_expanded("1 ? 3 : 4").unwrap(), 3);
        assert_eq!(eval_expanded("0x10").unwrap(), 16);
        assert_eq!(eval_expanded("199901L >= 199901L").unwrap(), 1);
        assert_eq!(eval_expanded("!0").unwrap(), 1);
        assert_eq!(eval_expanded("'A' == 65").unwrap(), 1);
    }
}
