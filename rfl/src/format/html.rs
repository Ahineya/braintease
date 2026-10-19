//! Layout for the standard HTML DSL only. No macro execution or import loading.
//! Raw text is opaque; only tag trivia, RFL expressions and layout-only child
//! whitespace may change. The parsed tree is compared after printing.
use crate::macros::syntax::{layout_trees, TokenTree, TreeKind};
use crate::{lexer, CompileError};
use std::collections::HashSet;

#[derive(Debug)]
enum Child {
    Element(Element),
    Expression(String),
    Text(String),
}
#[derive(Debug)]
enum Property {
    Value(String, String, bool),
    Comment(String),
}
#[derive(Debug)]
struct Element {
    name: String,
    props: Vec<Property>,
    children: Vec<Child>,
    close: String,
    self_closing: bool,
    preserved: Option<String>,
}
struct Parser<'a> {
    source: &'a str,
    tokens: Vec<TokenTree>,
    at: usize,
}
fn invalid() -> CompileError {
    CompileError::plain("invalid HTML macro syntax; source was left unchanged")
}
fn comment(token: &TokenTree) -> bool {
    matches!(token.kind, TreeKind::Trivia)
        && (token.spelling.starts_with("//") || token.spelling.starts_with("/*"))
}
fn layout(text: &str) -> bool {
    text.contains(['\n', '\r']) && text.chars().all(|c| matches!(c, ' ' | '\t' | '\r' | '\n'))
}
impl Parser<'_> {
    fn is(&self, s: &str) -> bool {
        self.tokens.get(self.at).is_some_and(|t| t.spelling == s)
    }
    fn take(&mut self, s: &str) -> Result<(), CompileError> {
        if !self.is(s) {
            return Err(invalid());
        }
        self.at += 1;
        Ok(())
    }
    fn whitespace(&mut self) {
        while self
            .tokens
            .get(self.at)
            .is_some_and(|t| matches!(t.kind, TreeKind::Trivia) && !comment(t))
        {
            self.at += 1;
        }
    }
    fn name(&mut self) -> Result<String, CompileError> {
        let t = self.tokens.get(self.at).ok_or_else(invalid)?;
        if !matches!(t.kind, TreeKind::Ident) {
            return Err(invalid());
        }
        self.at += 1;
        Ok(t.spelling.clone())
    }
    fn element(&mut self, depth: usize, preserve: bool) -> Result<Element, CompileError> {
        if depth > 128 {
            return Err(invalid());
        }
        self.take("<")?;
        let name = if self.is(">") {
            String::new()
        } else {
            self.name()?
        };
        let mut props = Vec::new();
        loop {
            self.whitespace();
            if self.is(">") || self.is("/") {
                break;
            }
            if self.tokens.get(self.at).is_some_and(comment) {
                props.push(Property::Comment(self.tokens[self.at].spelling.clone()));
                self.at += 1;
                continue;
            }
            let mut name = self.name()?;
            while self.is("-") {
                self.at += 1;
                name.push('-');
                name.push_str(&self.name()?);
            }
            self.whitespace();
            self.take("=")?;
            self.whitespace();
            let token = self.tokens.get(self.at).ok_or_else(invalid)?;
            let expression = matches!(token.kind, TreeKind::Group { delimiter: '{', .. });
            if !expression && !matches!(token.kind, TreeKind::Literal) {
                return Err(invalid());
            }
            props.push(Property::Value(name, token.spelling.clone(), expression));
            self.at += 1;
        }
        let self_closing = self.is("/");
        if self_closing {
            self.at += 1;
        }
        self.take(">")?;
        let preserve = preserve || matches!(name.as_str(), "pre" | "textarea");
        let content_start = self.tokens[self.at - 1].span.range.end;
        let mut children = Vec::new();
        let mut close = String::new();
        let mut preserved = None;
        if !self_closing {
            loop {
                let token = self.tokens.get(self.at).ok_or_else(invalid)?;
                if self.is("<")
                    && self
                        .tokens
                        .get(self.at + 1)
                        .is_some_and(|t| t.spelling == "/")
                {
                    let close_start = token.span.range.start;
                    self.at += 2;
                    let closing = if self.is(">") {
                        String::new()
                    } else {
                        self.name()?
                    };
                    if closing != name {
                        return Err(invalid());
                    }
                    self.whitespace();
                    self.take(">")?;
                    close =
                        self.source[close_start..self.tokens[self.at - 1].span.range.end].into();
                    if preserve {
                        preserved = Some(self.source[content_start..close_start].into());
                    }
                    break;
                }
                if self.is("<") {
                    children.push(Child::Element(self.element(depth + 1, preserve)?));
                } else if matches!(token.kind, TreeKind::Group { delimiter: '{', .. }) {
                    children.push(Child::Expression(token.spelling.clone()));
                    self.at += 1;
                } else {
                    let begin = token.span.range.start;
                    let mut end = begin;
                    while let Some(t) = self.tokens.get(self.at) {
                        if t.spelling == "<"
                            || matches!(t.kind, TreeKind::Group { delimiter: '{', .. })
                        {
                            break;
                        }
                        end = t.span.range.end;
                        self.at += 1;
                    }
                    let text = &self.source[begin..end];
                    if preserve || !layout(text) {
                        children.push(Child::Text(text.into()));
                    }
                }
            }
        }
        Ok(Element {
            name,
            props,
            children,
            close,
            self_closing,
            preserved,
        })
    }
}
fn parse(group: &str) -> Result<Element, CompileError> {
    let body = group
        .get(1..group.len().saturating_sub(1))
        .ok_or_else(invalid)?;
    let mut parser = Parser {
        source: body,
        tokens: layout_trees(body)?,
        at: 0,
    };
    // Root trivia is ignored by the standard macro, including comments. Keep
    // comments untouched by declining to lay out this uncommon form.
    parser.whitespace();
    let root = parser.element(0, false)?;
    parser.whitespace();
    if parser.at != parser.tokens.len() {
        return Err(invalid());
    }
    Ok(root)
}
fn html_call(tokens: &[lexer::Token], at: usize, names: &HashSet<String>) -> bool {
    let mut previous = tokens[..at].iter().rev().filter(|t| {
        !matches!(
            t.kind,
            lexer::TokenKind::LineComment | lexer::TokenKind::BlockComment
        )
    });
    previous
        .next()
        .is_some_and(|t| t.kind == lexer::TokenKind::Not)
        && previous.next().is_some_and(
            |t| matches!(&t.kind, lexer::TokenKind::Ident(name) if names.contains(name)),
        )
}

fn expression_key(value: &str, names: &HashSet<String>) -> Option<Vec<String>> {
    let tokens = lexer::tokenize_with_trivia(value).ok()?;
    let mut out = Vec::new();
    for (i, token) in tokens.iter().enumerate() {
        if let lexer::TokenKind::MacroBody(group) = &token.kind {
            if html_call(&tokens, i, names) {
                if let Ok(node) = parse(&group.text) {
                    node.key(&mut out, names);
                    continue;
                }
            }
        }
        out.push(format!("{:?}", token.kind));
    }
    Some(out)
}

// Reindent only syntactic layout. Multiline opaque macro bodies and HTML text
// are as significant as multiline comments/literals, even inside nested views.
fn verbatim_expression(value: &str, names: &HashSet<String>) -> bool {
    let Ok(tokens) = lexer::tokenize_with_trivia(value) else {
        return true;
    };
    tokens.iter().enumerate().any(|(i, token)| {
        if !value[token.span.start..token.span.end].contains(['\r', '\n']) {
            return false;
        }
        match &token.kind {
            lexer::TokenKind::BlockComment
            | lexer::TokenKind::Str(_)
            | lexer::TokenKind::CStr(_) => true,
            lexer::TokenKind::MacroBody(group) => {
                !html_call(&tokens, i, names)
                    || parse(&group.text).map_or(true, |node| node.verbatim(names))
            }
            _ => false,
        }
    })
}

impl Element {
    fn verbatim(&self, names: &HashSet<String>) -> bool {
        let multiline = |text: &str| text.contains(['\n', '\r']);
        self.preserved.as_deref().is_some_and(multiline)
            || self.props.iter().any(|p| match p {
                Property::Comment(text) => multiline(text),
                Property::Value(_, text, true) => verbatim_expression(text, names),
                Property::Value(_, text, false) => multiline(text),
            })
            || self.children.iter().any(|child| match child {
                Child::Element(node) => node.verbatim(names),
                Child::Text(text) => multiline(text),
                Child::Expression(text) => verbatim_expression(text, names),
            })
    }

    fn key(&self, out: &mut Vec<String>, names: &HashSet<String>) {
        out.push(format!("tag:{}:{}", self.name, self.self_closing));
        for prop in &self.props {
            match prop {
                Property::Value(name, value, expr) => out.push(format!(
                    "prop:{name}:{}",
                    if *expr {
                        format!("{:?}", expression_key(value, names))
                    } else {
                        value.clone()
                    }
                )),
                Property::Comment(text) => out.push(format!("comment:{text}")),
            }
        }
        if let Some(text) = &self.preserved {
            out.push(format!("preserved:{text}"));
        } else {
            for child in &self.children {
                match child {
                    Child::Element(node) => node.key(out, names),
                    Child::Expression(text) => {
                        out.push(format!("expr:{:?}", expression_key(text, names)))
                    }
                    Child::Text(text) => out.push(format!("text:{text}")),
                }
            }
        }
        out.push(format!("close:{}", self.close));
    }
    fn render(&self, indent: usize, newline: &str, names: &HashSet<String>) -> String {
        let pad = "    ".repeat(indent);
        let mut out = format!("<{}", self.name);
        let values: Vec<_> = self
            .props
            .iter()
            .map(|p| match p {
                Property::Value(name, value, expr) => format!(
                    "{name}={}",
                    if *expr {
                        expression(value, indent + 1, newline, names)
                    } else {
                        value.clone()
                    }
                ),
                Property::Comment(text) => text.clone(),
            })
            .collect();
        let multiline = values.len() >= 3
            || values
                .iter()
                .any(|v| v.contains(['\r', '\n']) || v.starts_with("//"));
        for value in values {
            if multiline {
                out.push_str(newline);
                out.push_str(&pad);
                out.push_str("    ");
            } else {
                out.push(' ');
            }
            out.push_str(&value);
        }
        if multiline {
            out.push_str(newline);
            out.push_str(&pad);
        }
        if self.self_closing {
            out.push_str("/>");
            return out;
        }
        out.push('>');
        if let Some(text) = &self.preserved {
            out.push_str(text);
        } else {
            let block = !self.children.iter().any(|c| matches!(c, Child::Text(_)))
                && (self.children.len() > 1
                    || self.children.iter().any(|c| matches!(c, Child::Element(_))));
            for child in &self.children {
                let level = if block { indent + 1 } else { indent };
                if block {
                    out.push_str(newline);
                    out.push_str(&"    ".repeat(level));
                }
                out.push_str(&match child {
                    Child::Element(node) => node.render(level, newline, names),
                    Child::Expression(text) => expression(text, level, newline, names),
                    Child::Text(text) => text.clone(),
                });
            }
            if block {
                out.push_str(newline);
                out.push_str(&pad);
            }
        }
        out.push_str(&self.close);
        out
    }
}
fn expression(value: &str, indent: usize, newline: &str, names: &HashSet<String>) -> String {
    let inner = &value[1..value.len() - 1];
    if verbatim_expression(inner, names) {
        return value.into();
    }
    let mut imports: Vec<_> = names.iter().collect();
    imports.sort();
    let prefix = imports
        .iter()
        .map(|name| format!("import macro {{html as {name}}} from \"std/html_macro\";\n"))
        .collect::<String>();
    let wrapped = format!("{prefix}fn __html_format() {{ let __html_value = {inner}\n; }}");
    let Ok(formatted) = super::format_source(&wrapped) else {
        return value.into();
    };
    let Some((_, tail)) = formatted.split_once("let __html_value = ") else {
        return value.into();
    };
    let ending = if formatted.contains("\r\n") {
        ";\r\n}\r\n"
    } else {
        ";\n}\n"
    };
    let Some(tail) = tail.strip_suffix(ending) else {
        return value.into();
    };
    let lines: Vec<_> = tail
        .lines()
        .map(|line| line.strip_prefix("    ").unwrap_or(line))
        .collect();
    if lines.len() == 1 {
        return format!("{{{}}}", lines[0]);
    }
    let pad = "    ".repeat(indent);
    format!(
        "{{{newline}{}{newline}{pad}}}",
        lines
            .iter()
            .map(|l| format!("{pad}    {l}"))
            .collect::<Vec<_>>()
            .join(newline)
    )
}
pub(super) fn format(
    group: &str,
    indent: usize,
    newline: &str,
    names: &HashSet<String>,
) -> Result<String, CompileError> {
    let Ok(root) = parse(group) else {
        return Ok(group.into());
    };
    let pad = "    ".repeat(indent);
    Ok(format!(
        "{}{newline}{pad}    {}{newline}{pad}{}",
        &group[..1],
        root.render(indent + 1, newline, names),
        &group[group.len() - 1..]
    ))
}
pub(super) fn equivalent(before: &str, after: &str, names: &HashSet<String>) -> bool {
    if before == after {
        return true;
    }
    let (Ok(a), Ok(b)) = (parse(before), parse(after)) else {
        return false;
    };
    let (mut ak, mut bk) = (Vec::new(), Vec::new());
    a.key(&mut ak, names);
    b.key(&mut bk, names);
    ak == bk
}
