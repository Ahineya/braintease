//! C99 `#if` / `#elif` integer constant expression evaluator.

use anyhow::{anyhow, Result};

#[derive(Debug, Clone, PartialEq)]
enum Tok {
    Number(Integer),
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
        let mut text = String::new();
        let radix = if self.peek() == '0' && matches!(self.peek_at(1), 'x' | 'X') {
            self.bump(); self.bump();
            while self.peek().is_ascii_hexdigit() { text.push(self.bump()); }
            16
        } else {
            while self.peek().is_ascii_digit() { text.push(self.bump()); }
            if text.starts_with('0') && text.len() > 1 { 8 } else { 10 }
        };
        let mut unsigned = false;
        while matches!(self.peek(), 'u' | 'U' | 'l' | 'L') {
            unsigned |= matches!(self.bump(), 'u' | 'U');
        }
        let bits = u64::from_str_radix(&text, radix).map_err(|_| anyhow!("Invalid integer constant in #if: {text}"))?;
        unsigned |= radix != 10 && bits > i64::MAX as u64;
        if !unsigned && bits > i64::MAX as u64 { return Err(anyhow!("Signed integer constant too large in #if")); }
        Ok(Tok::Number(Integer { bits, unsigned }))
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
                'a' => 7,
                'b' => 8,
                'f' => 12,
                'v' => 11,
                '0' => 0,
                '\\' => b'\\' as i64,
                '\'' => b'\'' as i64,
                '"' => b'"' as i64,
                '?' => b'?' as i64,
                other => other as i64,
            }
        } else {
            self.bump() as i64
        };
        if self.peek() != '\'' {
            return Err(anyhow!("Unterminated character constant in #if"));
        }
        self.bump();
        Ok(Tok::Number(Integer::signed(value)))
    }
}

/// C99 preprocessing arithmetic uses intmax_t/uintmax_t (both 64 bits here).
#[derive(Debug, Clone, Copy, PartialEq)]
struct Integer { bits: u64, unsigned: bool }
impl Integer {
    fn signed(n: i64) -> Self { Self { bits: n as u64, unsigned: false } }
    fn truth(self) -> bool { self.bits != 0 }
}

struct Parser { tokens: Vec<Tok>, pos: usize }
impl Parser {
    fn peek(&self) -> &Tok { self.tokens.get(self.pos).unwrap_or(&Tok::Eof) }
    fn bump(&mut self) -> Tok { let t = self.peek().clone(); self.pos += 1; t }
    fn expect(&mut self, token: Tok, what: &str) -> Result<()> {
        if self.peek() == &token { self.bump(); Ok(()) }
        else { Err(anyhow!("Expected {what} in #if expression, got {:?}", self.peek())) }
    }
    fn precedence(t: &Tok) -> Option<u8> {
        Some(match t {
            Tok::OrOr => 1, Tok::AndAnd => 2, Tok::BitOr => 3, Tok::BitXor => 4,
            Tok::BitAnd => 5, Tok::Eq | Tok::Ne => 6,
            Tok::Lt | Tok::Gt | Tok::Le | Tok::Ge => 7,
            Tok::Shl | Tok::Shr => 8, Tok::Plus | Tok::Minus => 9,
            Tok::Star | Tok::Slash | Tok::Percent => 10, _ => return None,
        })
    }
    fn unary(&mut self, evaluate: bool) -> Result<Integer> {
        match self.bump() {
            Tok::Number(n) => Ok(n), Tok::Ident(_) => Ok(Integer::signed(0)),
            Tok::Plus => self.unary(evaluate),
            Tok::Minus => { let mut n = self.unary(evaluate)?; n.bits = n.bits.wrapping_neg(); Ok(n) }
            Tok::BitNot => { let mut n = self.unary(evaluate)?; n.bits = !n.bits; Ok(n) }
            Tok::Not => Ok(Integer::signed((!self.unary(evaluate)?.truth()) as i64)),
            Tok::LParen => { let n = self.expression(0, evaluate)?; self.expect(Tok::RParen, "')'")?; Ok(n) }
            t => Err(anyhow!("Unexpected token {t:?} in #if expression")),
        }
    }
    fn expression(&mut self, minimum: u8, evaluate: bool) -> Result<Integer> {
        let mut left = self.unary(evaluate)?;
        while let Some(precedence) = Self::precedence(self.peek()) {
            if precedence < minimum { break; }
            let op = self.bump();
            let active = evaluate && !(op == Tok::AndAnd && !left.truth() || op == Tok::OrOr && left.truth());
            let right = self.expression(precedence + 1, active)?;
            left = Self::binary(op, left, right, evaluate)?;
        }
        if minimum == 0 && self.peek() == &Tok::Question {
            self.bump();
            let yes = self.expression(0, evaluate && left.truth())?;
            self.expect(Tok::Colon, "':'")?;
            let no = self.expression(0, evaluate && !left.truth())?;
            let unsigned = yes.unsigned || no.unsigned;
            left = if left.truth() { yes } else { no };
            left.unsigned = unsigned;
        }
        Ok(left)
    }
    fn binary(op: Tok, a: Integer, b: Integer, evaluate: bool) -> Result<Integer> {
        let unsigned = a.unsigned || b.unsigned;
        let x = a.bits; let y = b.bits;
        let signed_x = x as i64; let signed_y = y as i64;
        let boolean = |v| Ok(Integer::signed(v as i64));
        let bits = match op {
            Tok::AndAnd => return boolean(a.truth() && b.truth()),
            Tok::OrOr => return boolean(a.truth() || b.truth()),
            Tok::Eq => return boolean(x == y), Tok::Ne => return boolean(x != y),
            Tok::Lt => return boolean(if unsigned { x < y } else { signed_x < signed_y }),
            Tok::Gt => return boolean(if unsigned { x > y } else { signed_x > signed_y }),
            Tok::Le => return boolean(if unsigned { x <= y } else { signed_x <= signed_y }),
            Tok::Ge => return boolean(if unsigned { x >= y } else { signed_x >= signed_y }),
            Tok::Plus => x.wrapping_add(y), Tok::Minus => x.wrapping_sub(y), Tok::Star => x.wrapping_mul(y),
            Tok::BitAnd => x & y, Tok::BitOr => x | y, Tok::BitXor => x ^ y,
            Tok::Shl | Tok::Shr => {
                if y >= 64 { if evaluate { return Err(anyhow!("Invalid shift count in #if expression")); } else { return Ok(a); } }
                return Ok(Integer { bits: if op == Tok::Shl { x.wrapping_shl(y as u32) }
                    else if a.unsigned { x >> y } else { (signed_x >> y) as u64 }, unsigned: a.unsigned });
            }
            Tok::Slash | Tok::Percent => {
                if y == 0 { if evaluate { return Err(anyhow!("Division by zero in #if expression")); } else { return Ok(Integer { bits: 0, unsigned }); } }
                if op == Tok::Slash { if unsigned { x / y } else { signed_x.wrapping_div(signed_y) as u64 } }
                else if unsigned { x % y } else { signed_x.wrapping_rem(signed_y) as u64 }
            }
            _ => unreachable!(),
        };
        Ok(Integer { bits, unsigned })
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
    let value = parser.expression(0, true)?;
    if !matches!(parser.peek(), Tok::Eof) {
        return Err(anyhow!(
            "Trailing tokens in #if expression starting at {:?}",
            parser.peek()
        ));
    }
    Ok(value.bits as i64)
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

#[cfg(test)]
mod wide_tests {
    use super::eval_expanded as eval;
    #[test]
    fn unsigned_intmax_constants_and_conversions() {
        for e in ["18446744073709551615ULL > 0", "0xffffffffffffffff > 0", "-1 < 1U == 0", "(18446744073709551615ULL >> 63) == 1", "(0 ? 1U : -1) > 0", "(1 ? -1 : 1U) > 0", "18446744073709551615ULL / 3 == 6148914691236517205ULL", "(18446744073709551615ULL + 1) == 0", "(1 || 1/0)", "(0 && 1/0) == 0", "1 ? 7 : 1/0"] {
            assert_ne!(eval(e).unwrap(), 0, "{e}");
        }
        assert!(eval("18446744073709551616ULL").is_err());
        assert!(eval("1 / 0").is_err());
    }
}
