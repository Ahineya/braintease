//! Source identity/provenance stays out of the legacy byte-range Span layout.
use super::syntax::{ExpansionId, SourceId, SyntaxContext, SyntaxSpan, TokenTree, TreeKind};
use crate::{ast::Span, CompileError};
use std::path::{Path, PathBuf};
#[derive(Debug, Clone)]
pub struct SourceFile {
    pub path: PathBuf,
    pub text: String,
}
#[derive(Debug, Clone)]
pub struct Mapping {
    pub generated: Span,
    pub origin: SyntaxSpan,
    pub call: Span,
    pub expansion: ExpansionId,
}
#[derive(Debug, Clone)]
pub struct Expansion {
    pub parent: Option<ExpansionId>,
    pub name: String,
    pub call: SyntaxSpan,
    pub definition: SourceId,
}
#[derive(Debug, Clone, Default)]
pub struct ExpansionData {
    pub files: Vec<SourceFile>,
    pub mappings: Vec<Mapping>,
    pub calls: Vec<(String, Span)>,
    pub expansions: std::collections::HashMap<ExpansionId, Expansion>,
    pub names: std::collections::HashMap<String, String>,
}
impl ExpansionData {
    pub fn source(&mut self, path: &Path, text: &str) -> SourceId {
        if let Some(i) = self
            .files
            .iter()
            .position(|f| f.path == path && f.text == text)
        {
            return SourceId(i as u32);
        }
        let id = SourceId(self.files.len() as u32);
        self.files.push(SourceFile {
            path: path.into(),
            text: text.into(),
        });
        id
    }
    pub fn at(&self, span: Span) -> Option<&Mapping> {
        self.mappings
            .iter()
            .rev()
            .find(|m| m.generated.start <= span.start && span.start < m.generated.end)
    }
    pub fn diagnostic(&self, err: CompileError) -> CompileError {
        let Some(at) = err.offset else {
            return err;
        };
        let Some(m) = self.at(Span::new(at, at + 1)) else {
            if self.files.first().is_some_and(|f| at > f.text.len()) {
                if let Some((id, record)) = self.expansions.iter().max_by_key(|(id, _)| id.0) {
                    let file = &self.files[record.call.source.0 as usize];
                    let mut error = CompileError::at_file(
                        &file.path,
                        &file.text,
                        record.call.range.start,
                        err.detail,
                    );
                    self.annotate(&mut error, *id);
                    return error;
                }
            }
            return err;
        };
        let origin = &self.files[m.origin.source.0 as usize];
        let mut error =
            CompileError::at_file(&origin.path, &origin.text, m.origin.range.start, err.detail);
        for (internal, name) in &self.names {
            error.detail = error.detail.replace(internal, name);
            error.message = error.message.replace(internal, name);
        }
        self.annotate(&mut error, m.expansion);
        error
    }
    pub fn annotate(&self, error: &mut CompileError, mut id: ExpansionId) {
        for _ in 0..8 {
            let Some(record) = self.expansions.get(&id) else {
                return;
            };
            let source = &self.files[record.call.source.0 as usize];
            let (line, col) = crate::error::line_col(&source.text, record.call.range.start);
            error.message.push_str(&format!(
                "\nnote: expanding `{}` at {}:{line}:{col} (defined in {})\n",
                record.name,
                source.path.display(),
                self.files[record.definition.0 as usize].path.display()
            ));
            if let Some(parent) = record.parent {
                id = parent;
            } else {
                return;
            }
        }
        error
            .message
            .push_str("note: further expansion context omitted\n");
    }
    pub fn append(
        &mut self,
        text: &mut String,
        trees: &[TokenTree],
        call: Span,
        expansion: ExpansionId,
    ) -> Span {
        text.push('\n');
        let start = text.len();
        fn emit(
            data: &mut ExpansionData,
            text: &mut String,
            trees: &[TokenTree],
            call: Span,
            expansion: ExpansionId,
        ) {
            let mut previous_joint = true;
            let mut previous_punct = false;
            for t in trees {
                if previous_punct && !previous_joint && matches!(t.kind, TreeKind::Punct { .. }) {
                    text.push(' ');
                }
                previous_joint = matches!(t.kind, TreeKind::Punct { joint: true });
                previous_punct = matches!(t.kind, TreeKind::Punct { .. });
                // Separate independent word tokens. Existing trivia/joint
                // punctuation retain their spelling and evaluation syntax.
                if text
                    .chars()
                    .last()
                    .is_some_and(|c| c.is_ascii_alphanumeric() || c == '_')
                    && t.spelling
                        .chars()
                        .next()
                        .is_some_and(|c| c.is_ascii_alphanumeric() || c == '_')
                {
                    text.push(' ');
                }
                let start = text.len();
                if let TreeKind::Group {
                    delimiter,
                    children,
                } = &t.kind
                {
                    text.push(*delimiter);
                    data.mappings.push(Mapping {
                        generated: Span::new(start, text.len()),
                        origin: t.span,
                        call,
                        expansion,
                    });
                    emit(data, text, children, call, expansion);
                    let begin = text.len();
                    text.push(match delimiter {
                        '(' => ')',
                        '[' => ']',
                        _ => '}',
                    });
                    data.mappings.push(Mapping {
                        generated: Span::new(begin, text.len()),
                        origin: t.span,
                        call,
                        expansion,
                    });
                } else {
                    text.push_str(&t.spelling);
                    data.mappings.push(Mapping {
                        generated: Span::new(start, text.len()),
                        origin: t.span,
                        call,
                        expansion,
                    });
                }
            }
        }
        emit(self, text, trees, call, expansion);
        Span::new(start, text.len())
    }
    pub fn context(&self, span: Span) -> SyntaxContext {
        self.at(span).map_or(SyntaxContext(0), |m| m.origin.context)
    }
}

thread_local! {static OVERLAY:std::cell::RefCell<std::collections::BTreeMap<PathBuf,String>>=const{std::cell::RefCell::new(std::collections::BTreeMap::new())};}
pub struct Overlay(std::collections::BTreeMap<PathBuf, String>);
pub fn overlay(sources: std::collections::BTreeMap<PathBuf, String>) -> Overlay {
    Overlay(OVERLAY.with(|o| std::mem::replace(&mut *o.borrow_mut(), sources)))
}
impl Drop for Overlay {
    fn drop(&mut self) {
        OVERLAY.with(|o| *o.borrow_mut() = std::mem::take(&mut self.0));
    }
}
pub fn read(path: &Path) -> std::io::Result<String> {
    let path = crate::rename::canonical(path);
    if let Some(source) = OVERLAY.with(|o| o.borrow().get(&path).cloned()) {
        return Ok(source);
    }
    std::fs::read_to_string(path)
}
pub fn exists(path: &Path) -> bool {
    let path = crate::rename::canonical(path);
    path.exists() || OVERLAY.with(|o| o.borrow().contains_key(&path))
}

/// Saved files and named, unsaved editor modules share one normalized identity.
pub fn canonical(path: &Path) -> std::io::Result<PathBuf> {
    match std::fs::canonicalize(path) {
        Ok(path) => Ok(path),
        Err(error) => {
            let path = crate::rename::canonical(path);
            if OVERLAY.with(|o| o.borrow().contains_key(&path)) {
                Ok(path)
            } else {
                Err(error)
            }
        }
    }
}
