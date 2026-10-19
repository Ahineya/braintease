//! Project checked expansion facts back to editable source, never guessed types.
use super::source::ExpansionData;
use crate::{ast::Span, check::ProgramInfo};
impl ExpansionData {
    pub fn original(&self, span: Span) -> Span {
        let Some(first) = self.at(span) else {
            return span;
        };
        let last = self.at(Span::new(span.end.saturating_sub(1), span.end));
        if let Some(last) = last {
            if first.origin.source == last.origin.source
                && first.origin.range.start <= last.origin.range.end
            {
                return Span::new(first.origin.range.start, last.origin.range.end);
            }
        }
        first.call
    }
    pub fn editor(&self, mut info: ProgramInfo) -> ProgramInfo {
        let len = self.files[0].text.len();
        info.display_names.extend(self.names.clone());
        info.facts.retain_mut(|f| {
            if f.name.contains("$macro$") {
                return false;
            }
            f.def = self.original(f.def);
            f.scope = self.original(f.scope);
            if f.scope.end > len {
                f.scope.end = len;
            }
            f.visible_from = self
                .original(Span::new(f.visible_from, f.visible_from))
                .start;
            f.def.end <= len
        });
        info.uses.retain_mut(|u| {
            // Definition-site helper references have no user-editable spelling.
            if self.at(u.span).is_some_and(|m| m.origin.context.0 != 0) {
                return false;
            }
            u.span = self.original(u.span);
            if u.def_file.is_none() {
                u.def = self.original(u.def);
            }
            u.span.end <= len
        });
        for e in &mut info.expr_types {
            e.span = self.original(e.span);
        }
        info.expr_types.retain(|e| e.span.end <= len);
        for c in &mut info.call_sigs {
            c.span = self.original(c.span);
        }
        info.call_sigs.retain(|c| c.span.end <= len);
        for m in &mut info.trait_methods {
            m.receiver = self.original(m.receiver);
        }
        info.trait_methods.retain(|m| m.receiver.end <= len);
        for m in &mut info.type_members {
            m.scope = self.original(m.scope);
        }
        info
    }
}

/// Recover at a source-level declaration boundary, discarding only the damaged
/// declaration. We never pretend an incomplete DSL body was valid RFL.
pub fn recover(source: &str, error: &crate::CompileError) -> Option<String> {
    let at = error.offset?;
    if !error.detail.contains("macro") {
        return None;
    }
    let mut headers = Vec::new();
    let mut offset = 0;
    for line in source.split_inclusive('\n') {
        let trimmed = line.trim_start();
        if [
            "fn ",
            "async fn ",
            "export fn ",
            "export async fn ",
            "const ",
            "struct ",
            "type ",
            "trait ",
            "impl ",
            "import ",
            "export import ",
        ]
        .iter()
        .any(|p| trimmed.starts_with(p))
        {
            headers.push(offset);
        }
        offset += line.len();
    }
    let start = headers.iter().copied().filter(|&i| i <= at).last()?;
    let end = headers.iter().copied().find(|&i| i > at)?;
    let mut output = source.to_owned();
    let mask = source[start..end]
        .chars()
        .map(|ch| {
            if ch == '\n' || ch == '\r' {
                ch.to_string()
            } else {
                " ".repeat(ch.len_utf8())
            }
        })
        .collect::<String>();
    output.replace_range(start..end, &mask);
    Some(output)
}

/// Dependencies are hashed from content, including unsaved editor overlays.
/// This is an analysis invalidation key, not a cached expansion result.
pub fn fingerprint(path: Option<&std::path::Path>, source: &str) -> u64 {
    use std::hash::{Hash, Hasher};
    fn visit(
        path: &std::path::Path,
        source: &str,
        seen: &mut std::collections::HashSet<std::path::PathBuf>,
        hash: &mut std::collections::hash_map::DefaultHasher,
    ) {
        let path = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_owned());
        if !seen.insert(path.clone()) {
            return;
        }
        path.hash(hash);
        source.hash(hash);
        let manifest = path.with_extension("rfl.inputs");
        let declared = super::source::read(&manifest);
        declared.as_ref().ok().hash(hash);
        if let Ok(declared) = declared {
            for input in declared
                .lines()
                .map(str::trim)
                .filter(|n| !n.is_empty() && !n.starts_with('#'))
            {
                let input = path.parent().unwrap().join(input);
                input.hash(hash);
                super::source::read(&input).ok().hash(hash);
            }
        }
        let recovered;
        let syntax = match crate::lexer::tokenize(source) {
            Ok(_) => source,
            Err(error) => {
                let Some(repair) = recover(source, &error) else {
                    return;
                };
                recovered = repair;
                &recovered
            }
        };
        let Ok(tokens) = crate::lexer::tokenize(syntax) else {
            return;
        };
        for target in [crate::Target::Ripple, crate::Target::Native] {
            let ast = match crate::parser::parse_for_target(syntax, &tokens, target) {
                Ok(ast) => ast,
                Err(_) => {
                    let (_, errors) = crate::parser::parse_for_ide(syntax, &tokens);
                    let Some(repaired) = recover_parsed(syntax, &tokens, &errors) else {
                        continue;
                    };
                    let Ok(tokens) = crate::lexer::tokenize(&repaired) else {
                        continue;
                    };
                    let Ok(ast) = crate::parser::parse_for_target(&repaired, &tokens, target)
                    else {
                        continue;
                    };
                    ast
                }
            };
            for import in ast.imports.iter().chain(&ast.macro_imports) {
                import.path.hash(hash);
                if let Ok(dependency) = crate::modules::resolve_spec(&path, &import.path) {
                    if let Ok(text) = super::source::read(&dependency) {
                        visit(&dependency, &text, seen, hash);
                    }
                }
            }
        }
    }
    let mut hash = std::collections::hash_map::DefaultHasher::new();
    if let Some(path) = path {
        visit(
            path,
            source,
            &mut std::collections::HashSet::new(),
            &mut hash,
        );
    } else {
        source.hash(&mut hash);
    }
    hash.finish()
}

/// Refactoring keeps virtual positions for private generated bindings while
/// exposing only actual editable caller tokens to the existing binding index.
pub fn refactoring(
    ast: &mut crate::ast::Module,
    source: &str,
    tokens: &mut Vec<crate::lexer::Token>,
) -> Result<(), crate::CompileError> {
    let Some(data) = ast.macro_expansion.clone() else {
        return Ok(());
    };
    struct Project<'a> {
        data: &'a ExpansionData,
        source: &'a str,
        tokens: &'a mut Vec<crate::lexer::Token>,
    }
    impl super::walk::Visitor for Project<'_> {
        fn name(
            &mut self,
            name: &mut String,
            span: Span,
            _: super::walk::Role,
        ) -> Result<(), crate::CompileError> {
            let original = self.data.original(span);
            if self.data.context(span).0 == 0
                && self.source.get(original.start..original.end) == Some(name.as_str())
            {
                self.tokens.push(crate::lexer::Token {
                    kind: crate::lexer::TokenKind::Ident(name.clone()),
                    span: original,
                });
            }
            Ok(())
        }
        fn span(&mut self, span: &mut Span) {
            if self.data.context(*span).0 == 0 {
                *span = self.data.original(*span);
            }
        }
    }
    super::walk::module(
        &mut Project {
            data: &data,
            source,
            tokens,
        },
        ast,
    )?;
    // Resolver-only helper aliases have no editable import spelling.
    ast.imports.retain(|i| !i.is_implicit());
    tokens.sort_by_key(|t| (t.span.start, t.span.end));
    tokens.dedup();
    Ok(())
}

/// A missing macro close can consume the surrounding function's final brace.
/// Recover before the next declaration even when lexing that group succeeded.
pub fn recover_parsed(
    source: &str,
    tokens: &[crate::lexer::Token],
    errors: &[crate::CompileError],
) -> Option<String> {
    let mut headers = Vec::new();
    let mut offset = 0;
    for line in source.split_inclusive('\n') {
        let text = line.trim_start();
        if [
            "fn ",
            "async fn ",
            "export fn ",
            "export async fn ",
            "struct ",
            "type ",
            "const ",
        ]
        .iter()
        .any(|s| text.starts_with(s))
        {
            headers.push((offset, offset + line.len() - text.len()));
        }
        offset += line.len();
    }
    let mut damaged = None;
    for pair in headers.windows(2) {
        let start = pair[0].0;
        let end = pair[1].0;
        let keyword = pair[1].1;
        let local: Vec<_> = tokens
            .iter()
            .filter(|t| t.span.start >= start && t.span.end <= end)
            .collect();
        let balance = local.iter().fold(0i32, |n, t| {
            n + match t.kind {
                crate::lexer::TokenKind::LBrace => 1,
                crate::lexer::TokenKind::RBrace => -1,
                _ => 0,
            }
        });
        if balance <= 0
            || !local
                .iter()
                .any(|t| matches!(t.kind, crate::lexer::TokenKind::MacroBody(_)))
        {
            continue;
        }
        let body = source[keyword..]
            .find('{')
            .map_or(source.len(), |n| keyword + n);
        if errors.iter().filter_map(|e| e.offset).any(|at| {
            at >= keyword && at <= body
                || at <= keyword && at >= start && source[at..keyword].trim().is_empty()
        }) {
            damaged = Some((start, end));
            break;
        }
    }
    let (start, end) = damaged?;
    let mut result = source.to_owned();
    let mask = source[start..end]
        .chars()
        .map(|c| {
            if c == '\n' || c == '\r' {
                c.to_string()
            } else {
                " ".repeat(c.len_utf8())
            }
        })
        .collect::<String>();
    result.replace_range(start..end, &mask);
    Some(result)
}
