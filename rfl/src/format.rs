//! Opinionated, whitespace-only formatting shared by the CLI and editors.
//!
//! Syntax determines spacing and block layout. Token spellings (including
//! comments and escaped literals) are retained and checked before returning.

mod html;
mod syntax;

use crate::error::CompileError;
use crate::lexer::{self, Token, TokenKind as K};

/// A byte-indexed whitespace replacement in the original source.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormatEdit {
    pub start: usize,
    pub end: usize,
    pub new_text: String,
}

/// Format syntactically valid RFL with four spaces and no line-width limit.
/// Does not resolve imports or require successful type checking.
pub fn format_source(source: &str) -> Result<String, CompileError> {
    let tokens = lexer::tokenize_with_trivia(source)?;
    let syntax = syntax::Syntax::analyze(source, &tokens)?;
    let formatted = Printer::new(source, &tokens, &syntax).print()?;
    let after = lexer::tokenize_with_trivia(&formatted)?;
    if !same_tokens(source, &tokens, &formatted, &after, &syntax) {
        return Err(CompileError::plain(
            "formatting would change a token; source was left unchanged",
        ));
    }
    // Also catch whitespace-sensitive ambiguities in the parser.
    syntax::Syntax::analyze(&formatted, &after)?;
    Ok(formatted)
}

/// Minimal replacements confined to whitespace between tokens, in source order.
pub fn format_edits(source: &str) -> Result<Vec<FormatEdit>, CompileError> {
    let formatted = format_source(source)?;
    let mut edits = Vec::new();
    let (mut a, mut b) = (0, 0);
    while a < source.len() || b < formatted.len() {
        let old_start = a;
        let new_start = b;
        while source[a..].chars().next().is_some_and(char::is_whitespace) {
            a += source[a..].chars().next().unwrap().len_utf8();
        }
        while formatted[b..]
            .chars()
            .next()
            .is_some_and(char::is_whitespace)
        {
            b += formatted[b..].chars().next().unwrap().len_utf8();
        }
        let old_gap = &source[old_start..a];
        let new_gap = &formatted[new_start..b];
        if old_gap != new_gap {
            let prefix = old_gap
                .chars()
                .zip(new_gap.chars())
                .take_while(|(a, b)| a == b)
                .map(|(ch, _)| ch.len_utf8())
                .sum::<usize>();
            let suffix = old_gap[prefix..]
                .chars()
                .rev()
                .zip(new_gap[prefix..].chars().rev())
                .take_while(|(a, b)| a == b)
                .map(|(ch, _)| ch.len_utf8())
                .sum::<usize>();
            edits.push(FormatEdit {
                start: old_start + prefix,
                end: a - suffix,
                new_text: new_gap[prefix..new_gap.len() - suffix].to_string(),
            });
        }
        match (source[a..].chars().next(), formatted[b..].chars().next()) {
            (Some(x), Some(y)) if x == y => {
                a += x.len_utf8();
                b += y.len_utf8();
            }
            (None, None) => break,
            _ => {
                return Err(CompileError::plain(
                    "formatting would change non-whitespace content",
                ))
            }
        }
    }
    Ok(edits)
}

fn same_tokens(a: &str, at: &[Token], b: &str, bt: &[Token], syntax: &syntax::Syntax) -> bool {
    at.len() == bt.len()
        && at.iter().zip(bt).all(|(x, y)| {
            if syntax.html_bodies.contains(&x.span.start) {
                matches!((&x.kind, &y.kind), (K::MacroBody(_), K::MacroBody(_)))
                    && html::equivalent(
                        &a[x.span.start..x.span.end],
                        &b[y.span.start..y.span.end],
                        &syntax.html_names,
                    )
            } else {
                x.kind == y.kind && a[x.span.start..x.span.end] == b[y.span.start..y.span.end]
            }
        })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Layout {
    Block,
    InlineBrace,
    List,
    Inline,
    Attribute,
}

struct Frame {
    layout: Layout,
    empty: bool,
}

struct Printer<'a> {
    source: &'a str,
    tokens: &'a [Token],
    syntax: &'a syntax::Syntax,
    output: String,
    newline: &'static str,
    indent: usize,
    space: bool,
    frames: Vec<Frame>,
    closed: Option<Layout>,
    item_gap: bool,
}

impl<'a> Printer<'a> {
    fn new(source: &'a str, tokens: &'a [Token], syntax: &'a syntax::Syntax) -> Self {
        Self {
            source,
            tokens,
            syntax,
            output: String::new(),
            newline: if source.contains("\r\n") {
                "\r\n"
            } else {
                "\n"
            },
            indent: 0,
            space: false,
            frames: Vec::new(),
            closed: None,
            item_gap: false,
        }
    }

    fn bol(&self) -> bool {
        self.output.is_empty() || self.output.ends_with('\n')
    }

    fn word(&mut self, text: &str) {
        if self.bol() {
            self.output.push_str(&"    ".repeat(self.indent));
        } else if self.space && !self.output.ends_with(' ') {
            self.output.push(' ');
        }
        self.space = false;
        self.output.push_str(text);
    }

    fn line(&mut self) {
        self.space = false;
        if !self.bol() {
            // The lexer includes CR in a // token immediately before CRLF.
            if self.newline == "\r\n" && self.output.ends_with('\r') {
                self.output.push('\n');
            } else {
                self.output.push_str(self.newline);
            }
        }
    }

    fn blank(&mut self) {
        self.line();
        if !self.output.is_empty() && !self.output.ends_with(&self.newline.repeat(2)) {
            self.output.push_str(self.newline);
        }
    }

    fn trailing_comment(&self, i: usize) -> bool {
        self.tokens.get(i + 1).is_some_and(|next| {
            matches!(next.kind, K::LineComment | K::BlockComment)
                && !self.source[self.tokens[i].span.end..next.span.start].contains('\n')
        })
    }

    fn print(mut self) -> Result<String, CompileError> {
        for (i, token) in self.tokens.iter().enumerate() {
            if token.kind == K::Eof {
                break;
            }
            let prev = i.checked_sub(1).map(|p| &self.tokens[p].kind);
            let next = self.tokens.get(i + 1).map(|t| &t.kind);
            let text = &self.source[token.span.start..token.span.end];
            let closed = self.closed.take();
            if self.item_gap
                && !(matches!(token.kind, K::LineComment | K::BlockComment)
                    && i > 0
                    && !self.source[self.tokens[i - 1].span.end..token.span.start].contains('\n'))
            {
                if token.kind != K::Semi {
                    self.blank();
                }
                self.item_gap = false;
            }
            if self.syntax.lines.contains(&token.span.start) {
                self.line();
            }
            if self.bol() && i > 0 {
                let gap = &self.source[self.tokens[i - 1].span.end..token.span.start];
                if gap.bytes().filter(|b| *b == b'\n').count() >= 2 {
                    self.blank();
                }
            }
            match &token.kind {
                K::MacroBody(_) if self.syntax.html_bodies.contains(&token.span.start) => {
                    self.space = false;
                    let body =
                        html::format(text, self.indent, self.newline, &self.syntax.html_names)?;
                    self.word(&body);
                }
                K::LineComment | K::BlockComment => {
                    let inline = i > 0
                        && !self.source[self.tokens[i - 1].span.end..token.span.start]
                            .contains('\n');
                    if inline {
                        self.space = true;
                    } else {
                        self.line();
                    }
                    self.word(text);
                    if token.kind == K::LineComment
                        || next == Some(&K::Eof)
                        || self.tokens.get(i + 1).is_some_and(|t| {
                            self.source[token.span.end..t.span.start].contains('\n')
                        })
                    {
                        self.line();
                    } else {
                        self.space = true;
                    }
                }
                K::LBrace | K::LParen | K::LBracket => {
                    let layout = if token.kind == K::LBrace {
                        if self.syntax.inline_braces.contains(&token.span.start) {
                            Layout::InlineBrace
                        } else {
                            Layout::Block
                        }
                    } else if token.kind == K::LBracket && prev == Some(&K::Hash) {
                        Layout::Attribute
                    } else {
                        Layout::Inline
                    };
                    let end = self.syntax.pairs[&i];
                    let empty = end == i + 1;
                    let has_comment = self.tokens[i + 1..end]
                        .iter()
                        .any(|t| matches!(t.kind, K::LineComment | K::BlockComment));
                    let layout =
                        if has_comment && matches!(layout, Layout::Inline | Layout::InlineBrace) {
                            Layout::List
                        } else {
                            layout
                        };
                    if token.kind == K::LBrace {
                        self.space = !self.bol() && !matches!(prev, Some(K::LParen | K::LBracket));
                    } else {
                        self.space =
                            matches!(prev, Some(K::If | K::While | K::For)) || self.needs_space(i);
                        if matches!(prev, Some(K::Ident(_) | K::RParen | K::RBracket))
                            || (token.kind == K::LBracket
                                && matches!(
                                    prev,
                                    Some(
                                        K::Int(_)
                                            | K::Float(_)
                                            | K::Str(_)
                                            | K::Char(_)
                                            | K::True
                                            | K::False
                                            | K::Question
                                            | K::Await
                                            | K::RBrace
                                    )
                                ))
                        {
                            self.space = false;
                        }
                    }
                    self.word(text);
                    if matches!(layout, Layout::Block | Layout::List) && !empty {
                        self.indent += 1;
                        self.line();
                    } else if layout == Layout::InlineBrace && !empty {
                        self.space = true;
                    }
                    self.frames.push(Frame { layout, empty });
                }
                K::RBrace | K::RParen | K::RBracket => {
                    let frame = self.frames.pop().expect("validated delimiter pair");
                    if matches!(frame.layout, Layout::Block | Layout::List) && !frame.empty {
                        self.indent -= 1;
                        self.line();
                    } else {
                        self.space = frame.layout == Layout::InlineBrace && !frame.empty;
                    }
                    self.word(text);
                    self.closed = Some(frame.layout);
                    if frame.layout == Layout::Attribute {
                        self.line();
                    } else if frame.layout == Layout::Block {
                        if self.trailing_comment(i) {
                            self.space = true;
                            self.item_gap = self.frames.is_empty();
                        } else if !matches!(
                            next,
                            Some(
                                K::Else
                                    | K::Semi
                                    | K::Comma
                                    | K::RParen
                                    | K::RBracket
                                    | K::Dot
                                    | K::Question
                                    | K::ColonColon
                                    | K::LParen
                                    | K::LBracket
                                    | K::As
                                    | K::Plus
                                    | K::Minus
                                    | K::Star
                                    | K::Slash
                                    | K::Percent
                                    | K::EqEq
                                    | K::NotEq
                                    | K::Lt
                                    | K::Le
                                    | K::Gt
                                    | K::Ge
                                    | K::AndAnd
                                    | K::OrOr
                                    | K::Amp
                                    | K::Pipe
                                    | K::Caret
                                    | K::Shl
                                    | K::Shr
                            )
                        ) {
                            if self.frames.is_empty() && next != Some(&K::Eof) {
                                self.blank();
                            } else {
                                self.line();
                            }
                        }
                    }
                }
                K::Semi => {
                    self.space = false;
                    self.word(text);
                    if matches!(self.frames.last().map(|f| f.layout), Some(Layout::Inline))
                        || self.trailing_comment(i)
                    {
                        self.space = true;
                    } else {
                        self.line();
                    }
                }
                K::Comma => {
                    self.space = false;
                    self.word(text);
                    if self.trailing_comment(i) {
                        self.space = true;
                    } else if !self.syntax.generic_commas.contains(&token.span.start)
                        && matches!(
                            self.frames.last().map(|f| f.layout),
                            Some(Layout::Block | Layout::List)
                        )
                    {
                        self.line();
                    } else {
                        self.space = true;
                    }
                }
                _ => {
                    self.space = self.needs_space(i)
                        || (token.kind == K::Else && closed == Some(Layout::Block));
                    self.word(text);
                }
            }
        }
        // A source containing only whitespace has no content to terminate.
        if !self.output.is_empty() {
            while self.output.ends_with('\n') || self.output.ends_with('\r') {
                self.output.pop();
            }
            self.output.push_str(self.newline);
        }
        Ok(self.output)
    }

    fn needs_space(&self, i: usize) -> bool {
        use K::*;
        let Some(previous) = i.checked_sub(1).map(|p| &self.tokens[p]) else {
            return false;
        };
        let current = &self.tokens[i];
        let a = &previous.kind;
        let b = &current.kind;
        if self.syntax.joined.contains(&current.span.start) {
            return false;
        }
        if matches!(a, LineComment | BlockComment) {
            return true;
        }
        if matches!(b, Colon | ColonColon | Dot | Question | Comma | Semi)
            || matches!(a, ColonColon | Dot | Hash | LParen | LBracket)
        {
            return false;
        }
        if matches!(a, DotDot | DotDotEq) || matches!(b, DotDot | DotDotEq) {
            return false;
        }
        if self.syntax.tight.contains(&current.span.start) {
            // Keep separate angle tokens separate: joining them changes the
            // lexer's token stream to << or >>, even in a generic type.
            if matches!((a, b), (Lt, Lt) | (Gt, Gt)) {
                return true;
            }
            // A prefix operator still needs separation from a preceding word.
            if matches!(b, Amp | Star | Minus | Not | Tilde) {
                return (matches!(a, Amp) && matches!(b, Amp))
                    || (!matches!(a, LParen | LBracket | ColonColon)
                        && !self.syntax.tight.contains(&previous.span.start));
            }
            return matches!(b, Lt | Shl)
                && !matches!(a, Ident(_) | Impl | For | Lt | Shl | ColonColon);
        }
        if self.syntax.tight.contains(&previous.span.start) {
            return matches!(a, Gt | Shr) && !matches!(b, LParen | RParen | RBracket | ColonColon);
        }
        if matches!(b, Not | Tilde) {
            return !matches!(a, LParen | LBracket);
        }
        if matches!(a, Not | Tilde) {
            return false;
        }
        if matches!(a, LBrace) {
            return true;
        }
        if matches!(b, LParen) {
            return !matches!(a, Ident(_) | Fn | RParen | RBracket | Gt | Shr);
        }
        true
    }
}
