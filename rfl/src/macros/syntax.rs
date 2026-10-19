//! Lossless macro input. Ordinary RFL tokenization never interprets a DSL body.
use crate::{ast::Span, CompileError};

pub const MAX_DEPTH: usize = 128;
pub const MAX_TOKENS: usize = 100_000;
pub const MAX_BYTES: usize = 1_048_576;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawGroup {
    pub delimiter: char,
    /// Includes the delimiters. Whitespace and arbitrary DSL punctuation survive.
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SourceId(pub(super) u32);
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ExpansionId(pub(super) u64);
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SyntaxContext(pub(super) u64);
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SyntaxSpan {
    pub source: SourceId,
    pub range: Span,
    pub expansion: Option<ExpansionId>,
    pub context: SyntaxContext,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TreeKind {
    Ident,
    Literal,
    Punct {
        joint: bool,
    },
    Trivia,
    Text,
    Group {
        delimiter: char,
        children: Vec<TokenTree>,
    },
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TokenTree {
    pub kind: TreeKind,
    pub spelling: String,
    pub span: SyntaxSpan,
}
#[derive(Debug, Clone)]
pub struct Invocation {
    pub name: String,
    pub name_span: Span,
    pub span: Span,
    pub group: RawGroup,
    pub input: Option<Vec<TokenTree>>,
}

fn closer(ch: char) -> char {
    match ch {
        '(' => ')',
        '[' => ']',
        '{' => '}',
        _ => unreachable!(),
    }
}
fn next(source: &str, at: usize) -> Option<char> {
    source.get(at..)?.chars().next()
}

// Apostrophes in raw text are punctuation. Only a complete single-character
// literal (possibly escaped) is treated as a quoted region.
fn quoted_end(source: &str, start: usize) -> Result<Option<usize>, CompileError> {
    let quote = next(source, start).unwrap();
    if quote == '\'' {
        let mut at = start + 1;
        let Some(ch) = next(source, at) else {
            return Ok(None);
        };
        at += ch.len_utf8();
        if ch == '\\' {
            let Some(ch) = next(source, at) else {
                return Ok(None);
            };
            at += ch.len_utf8();
        }
        return Ok((next(source, at) == Some('\'')).then_some(at + 1));
    }
    let mut at = start + 1;
    while let Some(ch) = next(source, at) {
        at += ch.len_utf8();
        if ch == quote {
            return Ok(Some(at));
        }
        if ch == '\\' {
            let Some(ch) = next(source, at) else {
                break;
            };
            at += ch.len_utf8();
        }
    }
    Err(CompileError::at(
        source,
        start,
        "unterminated string in macro input",
    ))
}

fn comment_end(source: &str, start: usize) -> Result<Option<usize>, CompileError> {
    let text = &source[start..];
    if text.starts_with("//") {
        return Ok(Some(start + text.find('\n').unwrap_or(text.len())));
    }
    if text.starts_with("/*") {
        let mut depth = 1usize;
        let mut at = start + 2;
        while at < source.len() {
            if source[at..].starts_with("/*") {
                depth += 1;
                at += 2;
            } else if source[at..].starts_with("*/") {
                depth -= 1;
                at += 2;
                if depth == 0 {
                    return Ok(Some(at));
                }
            } else {
                at += next(source, at).unwrap().len_utf8();
            }
        }
        return Err(CompileError::at(
            source,
            start,
            "unterminated comment in macro input",
        ));
    }
    Ok(None)
}

pub fn capture(source: &str, start: usize) -> Result<(RawGroup, usize), CompileError> {
    let delimiter = next(source, start).unwrap();
    let mut stack = vec![closer(delimiter)];
    let mut at = start + 1;
    while let Some(ch) = next(source, at) {
        if at - start > MAX_BYTES {
            return Err(CompileError::at(
                source,
                start,
                "macro input exceeds byte limit",
            ));
        }
        if let Some(end) = comment_end(source, at)? {
            at = end;
            continue;
        }
        if matches!(ch, '"' | '\'') {
            if let Some(end) = quoted_end(source, at)? {
                at = end;
                continue;
            }
        }
        if matches!(ch, '(' | '[' | '{') {
            if stack.len() >= MAX_DEPTH {
                return Err(CompileError::at(
                    source,
                    at,
                    "macro delimiters exceed nesting limit",
                ));
            }
            stack.push(closer(ch));
        } else if matches!(ch, ')' | ']' | '}') {
            if stack.pop() != Some(ch) {
                return Err(CompileError::at(source, at, "mismatched macro delimiter"));
            }
            if stack.is_empty() {
                let end = at + 1;
                return Ok((
                    RawGroup {
                        delimiter,
                        text: source[start..end].into(),
                    },
                    end,
                ));
            }
        }
        at += ch.len_utf8();
    }
    Err(CompileError::at(source, start, "unclosed macro invocation"))
}

/// Read a balanced tree without throwing away text that is not valid RFL.
pub fn trees(
    source: &str,
    source_id: SourceId,
    context: SyntaxContext,
    base: usize,
) -> Result<Vec<TokenTree>, CompileError> {
    fn read(
        source: &str,
        at: &mut usize,
        close: Option<char>,
        depth: usize,
        count: &mut usize,
        id: SourceId,
        context: SyntaxContext,
        base: usize,
    ) -> Result<Vec<TokenTree>, CompileError> {
        if depth > MAX_DEPTH {
            return Err(CompileError::at(source, *at, "macro tree nesting limit"));
        }
        let mut result = Vec::new();
        while let Some(ch) = next(source, *at) {
            if Some(ch) == close {
                *at += ch.len_utf8();
                return Ok(result);
            }
            if matches!(ch, ')' | ']' | '}') {
                return Err(CompileError::at(source, *at, "mismatched macro delimiter"));
            }
            let start = *at;
            let kind;
            if let Some(end) = comment_end(source, start)? {
                *at = end;
                kind = TreeKind::Trivia;
            } else if ch.is_whitespace() {
                *at += ch.len_utf8();
                while next(source, *at).is_some_and(char::is_whitespace) {
                    *at += next(source, *at).unwrap().len_utf8();
                }
                kind = TreeKind::Trivia;
            } else if matches!(ch, '(' | '[' | '{') {
                *at += 1;
                let children = read(
                    source,
                    at,
                    Some(closer(ch)),
                    depth + 1,
                    count,
                    id,
                    context,
                    base,
                )?;
                kind = TreeKind::Group {
                    delimiter: ch,
                    children,
                };
            } else if matches!(ch, '"' | '\'') && quoted_end(source, start)?.is_some() {
                *at = quoted_end(source, start)?.unwrap();
                kind = TreeKind::Literal;
            } else if ch.is_ascii_alphabetic() || ch == '_' {
                *at += 1;
                while next(source, *at).is_some_and(|c| c.is_ascii_alphanumeric() || c == '_') {
                    *at += 1;
                }
                kind = TreeKind::Ident;
            } else if ch.is_ascii_digit() {
                *at += 1;
                while next(source, *at).is_some_and(|c| c.is_ascii_alphanumeric() || c == '.') {
                    *at += 1;
                }
                kind = TreeKind::Literal;
            } else {
                *at += ch.len_utf8();
                kind = if ch.is_ascii_punctuation() {
                    TreeKind::Punct {
                        joint: next(source, *at).is_some_and(|c| {
                            c.is_ascii_punctuation()
                                && !matches!(c, '(' | '[' | '{' | ')' | ']' | '}' | '"' | '\'')
                        }),
                    }
                } else {
                    TreeKind::Text
                };
            }
            *count += 1;
            if *count > MAX_TOKENS {
                return Err(CompileError::at(
                    source,
                    start,
                    "macro token limit exceeded",
                ));
            }
            result.push(TokenTree {
                kind,
                spelling: source[start..*at].into(),
                span: SyntaxSpan {
                    source: id,
                    range: Span::new(base + start, base + *at),
                    expansion: None,
                    context,
                },
            });
        }
        if close.is_some() {
            return Err(CompileError::at(source, *at, "unclosed macro tree"));
        }
        Ok(result)
    }
    if source.len() > MAX_BYTES {
        return Err(CompileError::plain("macro input exceeds byte limit"));
    }
    read(source, &mut 0, None, 0, &mut 0, source_id, context, base)
}

/// Lossless syntax for layout tools; these placeholder handles never enter an
/// expansion request or resolve identifiers.
pub(crate) fn layout_trees(source: &str) -> Result<Vec<TokenTree>, CompileError> {
    trees(source, SourceId(0), SyntaxContext(0), 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn raw_input_preserves_unicode_apostrophes_comments_groups_and_jointness() {
        let text = "{ <p title=\"🙂\">isn't @ RFL /* ] } */ {name >> 1} <!-- hi --> </p> }";
        let (group, end) = capture(text, 0).unwrap();
        assert_eq!(end, text.len());
        assert_eq!(group.text, text);
        let ts = trees(&text[1..text.len() - 1], SourceId(7), SyntaxContext(9), 1).unwrap();
        assert_eq!(
            ts.iter().map(|t| t.spelling.as_str()).collect::<String>(),
            text[1..text.len() - 1]
        );
        assert!(ts.iter().any(|t| matches!(t.kind, TreeKind::Group { .. })));
        let operators = trees(">> > > ::", SourceId(0), SyntaxContext(0), 0).unwrap();
        assert!(matches!(operators[0].kind, TreeKind::Punct { joint: true }));
        assert!(matches!(
            operators[1].kind,
            TreeKind::Punct { joint: false }
        ));
        assert!(ts
            .iter()
            .all(|t| t.span.source == SourceId(7) && t.span.context == SyntaxContext(9)));
    }
    #[test]
    fn malformed_and_excessive_delimiters_fail_without_recursing_unboundedly() {
        for text in ["{([)]}", "{\"oops}", "{/* oops}", "{abc"] {
            assert!(capture(text, 0).is_err(), "{text}");
        }
        assert!(capture(
            &format!("{}{}", "{".repeat(MAX_DEPTH + 1), "}".repeat(MAX_DEPTH + 1)),
            0
        )
        .is_err());
        assert!(trees(&"x ".repeat(MAX_TOKENS), SourceId(0), SyntaxContext(0), 0).is_err());
    }
}
