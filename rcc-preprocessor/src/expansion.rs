//! Token-based C99 macro replacement. Tokens carry a hide set so a macro
//! disabled during rescanning cannot become eligible again in a later pass.
//! Argument prescan retains this metadata; # and ## use the original tokens.
use crate::{Macro, Preprocessor};
use anyhow::{anyhow, bail, Result};
use std::collections::{BTreeSet, VecDeque};
use std::sync::Arc;

type HideSet = Arc<BTreeSet<String>>;
const MAX_ARGUMENT_DEPTH: usize = 256;
const MAX_EXPANSIONS: usize = 1_000_000;
const MAX_PENDING_TOKENS: usize = 2_000_000;

#[derive(Clone, Debug, PartialEq)]
enum Kind {
    Ident,
    Number,
    Literal,
    Punct,
    Space,
    Comment,
    Placemark,
}

#[derive(Clone, Debug)]
pub(crate) struct PpToken {
    kind: Kind,
    text: String,
    line: usize,
    hidden: HideSet,
}
impl PpToken {
    fn space(&self) -> bool {
        matches!(self.kind, Kind::Space | Kind::Comment)
    }
    fn is(&self, text: &str) -> bool {
        self.kind == Kind::Punct && self.text == text
    }
    fn with_text(&self, kind: Kind, text: String) -> Self {
        Self {
            kind,
            text,
            line: self.line,
            hidden: self.hidden.clone(),
        }
    }
}

/// Phase-3 preprocessing tokens, rather than C parser tokens: in particular a
/// pp-number includes exponent signs, and literal prefixes are indivisible.
fn tokenize(text: &str, mut line: usize) -> Result<Vec<PpToken>> {
    let mut tokens = Vec::new();
    let empty = HideSet::default();
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let start = i;
        let start_line = line;
        let quote_at = if matches!(bytes[i], b'"' | b'\'') {
            Some(i)
        } else if matches!(bytes[i], b'L' | b'u' | b'U')
            && matches!(bytes.get(i + 1), Some(b'"' | b'\''))
        {
            Some(i + 1)
        } else if text[i..].starts_with("u8\"") {
            Some(i + 2)
        } else {
            None
        };
        let kind = if let Some(q) = quote_at {
            let quote = bytes[q];
            i = q + 1;
            loop {
                let ch = *bytes
                    .get(i)
                    .ok_or_else(|| anyhow!("Unterminated preprocessing literal"))?;
                i += 1;
                if ch == b'\\' {
                    if i < bytes.len() {
                        i += 1;
                    }
                } else if ch == quote {
                    break;
                }
            }
            Kind::Literal
        } else if bytes[i].is_ascii_whitespace() {
            i += 1;
            while i < bytes.len() && bytes[i].is_ascii_whitespace() {
                i += 1;
            }
            Kind::Space
        } else if text[i..].starts_with("/*") {
            i += text[i..]
                .find("*/")
                .ok_or_else(|| anyhow!("Unterminated preprocessing comment"))?
                + 2;
            Kind::Comment
        } else if text[i..].starts_with("//") {
            i += text[i..].find('\n').unwrap_or(bytes.len() - i);
            Kind::Comment
        } else if text[i..]
            .chars()
            .next()
            .is_some_and(|c| c.is_alphabetic() || c == '_')
        {
            i += text[i..].chars().next().unwrap().len_utf8();
            while let Some(c) = text[i..].chars().next() {
                if !c.is_alphanumeric() && c != '_' {
                    break;
                }
                i += c.len_utf8();
            }
            Kind::Ident
        } else if bytes[i].is_ascii_digit()
            || (bytes[i] == b'.' && bytes.get(i + 1).is_some_and(u8::is_ascii_digit))
        {
            i += 1;
            while i < bytes.len() {
                if bytes[i].is_ascii_alphanumeric()
                    || matches!(bytes[i], b'.' | b'_')
                    || (matches!(bytes[i], b'+' | b'-')
                        && matches!(bytes[i - 1], b'e' | b'E' | b'p' | b'P'))
                {
                    i += 1;
                } else {
                    break;
                }
            }
            Kind::Number
        } else {
            // Longest match. Preserve otherwise unknown characters verbatim;
            // the C frontend diagnoses them if they survive preprocessing.
            const PUNCT: &[&str] = &[
                "%:%:", ">>=", "<<=", "...", "##", "->", "++", "--", "<<", ">>", "<=", ">=", "==",
                "!=", "&&", "||", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "<:", ":>", "<%",
                "%>", "%:",
            ];
            i += PUNCT
                .iter()
                .find(|p| text[i..].starts_with(**p))
                .map_or_else(|| text[i..].chars().next().unwrap().len_utf8(), |p| p.len());
            Kind::Punct
        };
        let spelling = &text[start..i];
        line += spelling.bytes().filter(|&b| b == b'\n').count();
        tokens.push(PpToken {
            kind,
            text: spelling.into(),
            line: start_line,
            hidden: empty.clone(),
        });
    }
    Ok(tokens)
}

fn trim(tokens: &mut Vec<PpToken>) {
    let start = tokens
        .iter()
        .position(|t| !t.space())
        .unwrap_or(tokens.len());
    tokens.drain(..start);
    while tokens.last().is_some_and(PpToken::space) {
        tokens.pop();
    }
}
fn union(a: &HideSet, b: &HideSet) -> HideSet {
    if a.is_empty() {
        return b.clone();
    }
    if b.is_empty() || Arc::ptr_eq(a, b) {
        return a.clone();
    }
    Arc::new(a.union(b).cloned().collect())
}

impl Preprocessor {
    pub fn expand_macros_impl(&self, text: &str) -> Result<String> {
        let tokens = tokenize(text, self.current_line)?;
        let mut budget = MAX_EXPANSIONS;
        let expanded = self.expand_tokens(tokens, 0, &mut budget)?;
        render(&expanded)
    }

    fn expand_tokens(
        &self,
        tokens: Vec<PpToken>,
        depth: usize,
        budget: &mut usize,
    ) -> Result<Vec<PpToken>> {
        if depth > MAX_ARGUMENT_DEPTH {
            bail!("Maximum macro argument expansion depth exceeded");
        }
        let mut input: VecDeque<_> = tokens.into();
        let mut output = Vec::new();
        while let Some(token) = input.pop_front() {
            if token.kind != Kind::Ident || token.hidden.contains(&token.text) {
                output.push(token);
                continue;
            }
            if matches!(token.text.as_str(), "__LINE__" | "__FILE__") {
                let (kind, text) = if token.text == "__LINE__" {
                    (Kind::Number, token.line.to_string())
                } else {
                    let file = self
                        .current_file
                        .as_ref()
                        .map(|p| p.display().to_string())
                        .unwrap_or_default();
                    (
                        Kind::Literal,
                        format!("\"{}\"", file.replace('\\', "\\\\").replace('"', "\\\"")),
                    )
                };
                output.push(token.with_text(kind, text));
                continue;
            }
            let Some(definition) = self.macros.get(&token.text) else {
                output.push(token);
                continue;
            };
            let (arguments, mut hidden) = if let Some(params) = &definition.params {
                let opening = input.iter().position(|t| !t.space());
                if !opening.is_some_and(|i| input[i].is("(")) {
                    output.push(token);
                    continue;
                }
                input.drain(..opening.unwrap() + 1);
                let (mut arguments, closing) = arguments(&mut input)?;
                if arguments.is_empty() && !params.is_empty() {
                    arguments.push(Vec::new());
                }
                if (!definition.is_variadic && arguments.len() != params.len())
                    || arguments.len() < params.len()
                {
                    bail!(
                        "Macro '{}' expects {}{} arguments, got {}",
                        definition.name,
                        if definition.is_variadic {
                            "at least "
                        } else {
                            ""
                        },
                        params.len(),
                        arguments.len()
                    );
                }
                // An invocation can cross replacement/input boundaries. Only
                // names disabled at both ends remain disabled for its body.
                let hidden = token
                    .hidden
                    .intersection(&closing.hidden)
                    .cloned()
                    .collect();
                (arguments, hidden)
            } else {
                (Vec::new(), (*token.hidden).clone())
            };
            if *budget == 0 {
                bail!("Macro expansion work limit exceeded");
            }
            *budget -= 1;
            hidden.insert(definition.name.clone());
            let hidden = Arc::new(hidden);
            let mut replacement =
                self.substitute(definition, arguments, token.line, depth, budget)?;
            for t in &mut replacement {
                t.hidden = union(&t.hidden, &hidden);
            }
            if input.len() + output.len() + replacement.len() > MAX_PENDING_TOKENS {
                bail!("Macro expansion token limit exceeded");
            }
            // Rescan the replacement together with the unconsumed input. Never
            // restart a whole-source pass or reconsider an emitted identifier.
            for t in replacement.into_iter().rev() {
                input.push_front(t);
            }
        }
        Ok(output)
    }

    fn substitute(
        &self,
        definition: &Macro,
        mut raw: Vec<Vec<PpToken>>,
        line: usize,
        depth: usize,
        budget: &mut usize,
    ) -> Result<Vec<PpToken>> {
        let params = definition.params.as_deref().unwrap_or_default();
        if definition.is_variadic {
            let tail = raw.split_off(params.len());
            let mut variadic = Vec::new();
            for (i, argument) in tail.into_iter().enumerate() {
                if i != 0 {
                    variadic.extend(tokenize(",", line)?);
                }
                variadic.extend(argument);
            }
            raw.push(variadic);
        }
        for argument in &mut raw {
            trim(argument);
        }
        let parameter = |t: &PpToken| {
            if t.kind != Kind::Ident {
                None
            } else if definition.is_variadic && t.text == "__VA_ARGS__" {
                Some(params.len())
            } else {
                params.iter().position(|p| *p == t.text)
            }
        };
        let body = self.macro_tokens[&definition.name]
            .get_or_try_init(|| tokenize(&definition.body, 0))?;
        let mut prescanned = vec![None; raw.len()];
        let mut pieces = Vec::new();
        let mut i = 0;
        while i < body.len() {
            let t = &body[i];
            let next = (i + 1..body.len()).find(|&j| !body[j].space());
            let previous = (0..i).rev().find(|&j| !body[j].space());
            if t.is("##") || t.is("%:%:") {
                pieces.push(Piece::Paste);
                i += 1;
                continue;
            }
            if (t.is("#") || t.is("%:")) && definition.params.is_some() {
                let p = next.and_then(|j| parameter(&body[j])).ok_or_else(|| {
                    anyhow!("Macro '{}': # must precede a parameter", definition.name)
                })?;
                let mut token = t.with_text(Kind::Literal, stringize(&raw[p]));
                token.line = line;
                pieces.push(Piece::Token(token));
                i = next.unwrap() + 1;
                continue;
            }
            if let Some(p) = parameter(t) {
                let pasted = next.is_some_and(|j| body[j].is("##") || body[j].is("%:%:"))
                    || previous.is_some_and(|j| body[j].is("##") || body[j].is("%:%:"));
                let values = if pasted {
                    &raw[p]
                } else {
                    // Lazy prescan: unused, stringized, and pasted arguments
                    // must not expand, even if expansion would be an error.
                    if prescanned[p].is_none() {
                        prescanned[p] =
                            Some(self.expand_tokens(raw[p].clone(), depth + 1, budget)?);
                    }
                    prescanned[p].as_ref().unwrap()
                };
                if pasted && values.is_empty() {
                    let mut empty = t.with_text(Kind::Placemark, String::new());
                    empty.line = line;
                    pieces.push(Piece::Token(empty));
                } else {
                    pieces.extend(values.iter().cloned().map(Piece::Token));
                }
            } else {
                let mut t = t.clone();
                t.line = line;
                pieces.push(Piece::Token(t));
            }
            i += 1;
        }
        paste(pieces, &definition.name)
    }
}

/// Arguments are collected before any expansion. Only parentheses shield
/// commas; strings/characters are already single tokens. An empty () is
/// normalized using the macro's parameter count by the caller.
fn arguments(input: &mut VecDeque<PpToken>) -> Result<(Vec<Vec<PpToken>>, PpToken)> {
    let mut args = Vec::new();
    let mut current = Vec::new();
    let mut nesting = 0;
    while let Some(t) = input.pop_front() {
        if t.is(")") && nesting == 0 {
            if current.iter().any(|t: &PpToken| !t.space()) || !args.is_empty() {
                args.push(current);
            }
            return Ok((args, t));
        }
        if t.is(",") && nesting == 0 {
            args.push(current);
            current = Vec::new();
        } else {
            if t.is("(") {
                nesting += 1;
            }
            if t.is(")") {
                nesting -= 1;
            }
            current.push(t);
        }
    }
    bail!("Unterminated macro arguments")
}

fn stringize(tokens: &[PpToken]) -> String {
    let mut result = String::from("\"");
    let mut pending_space = false;
    for t in tokens {
        if t.space() {
            pending_space = result.len() > 1;
            continue;
        }
        if pending_space {
            result.push(' ');
            pending_space = false;
        }
        if t.kind == Kind::Literal {
            result.push_str(&t.text.replace('\\', "\\\\").replace('"', "\\\""));
        } else {
            result.push_str(&t.text);
        }
    }
    result.push('"');
    result
}

enum Piece {
    Token(PpToken),
    Paste,
}
fn paste(pieces: Vec<Piece>, name: &str) -> Result<Vec<PpToken>> {
    let mut output: Vec<PpToken> = Vec::new();
    let mut input = pieces.into_iter();
    while let Some(piece) = input.next() {
        match piece {
            Piece::Token(t) => output.push(t),
            Piece::Paste => {
                while output.last().is_some_and(PpToken::space) {
                    output.pop();
                }
                let left = output
                    .pop()
                    .ok_or_else(|| anyhow!("Macro '{name}': ## has no left operand"))?;
                let right = loop {
                    match input.next() {
                        Some(Piece::Token(t)) if t.space() => continue,
                        Some(Piece::Token(t)) => break t,
                        _ => bail!("Macro '{name}': ## has no right operand"),
                    }
                };
                let result = if left.kind == Kind::Placemark {
                    right
                } else if right.kind == Kind::Placemark {
                    left
                } else {
                    let text = format!("{}{}", left.text, right.text);
                    let mut joined = tokenize(&text, left.line)?;
                    if joined.len() != 1 || joined[0].space() {
                        bail!("Macro '{name}': pasting '{}' and '{}' does not form one preprocessing token", left.text, right.text);
                    }
                    let mut t = joined.remove(0);
                    t.hidden = Arc::new(left.hidden.intersection(&right.hidden).cloned().collect());
                    t
                };
                output.push(result);
            }
        }
    }
    output.retain(|t| t.kind != Kind::Placemark);
    Ok(output)
}

/// Only ## may join preprocessing tokens. Insert a separator when textual
/// output would otherwise turn adjacent replacement tokens into a new token
/// (including ++, exponent numbers, wide literals, comments, and ellipses).
fn render(tokens: &[PpToken]) -> Result<String> {
    let mut output = String::new();
    let mut previous: Option<&PpToken> = None;
    for token in tokens {
        if !token.space() {
            if let Some(last) = previous {
                let joined = format!("{}{}", last.text, token.text);
                let retokenized = tokenize(&joined, 0);
                if (last.is(".") && token.is("."))
                    || retokenized.as_ref().map_or(true, |v| {
                        v.len() != 2 || v[0].text != last.text || v[1].text != token.text
                    })
                {
                    output.push(' ');
                }
            }
            previous = Some(token);
        } else {
            previous = None;
        }
        output.push_str(&token.text);
    }
    Ok(output)
}
