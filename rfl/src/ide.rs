//! Queries for the language server. Positions are byte offsets into `source`.
//! Scope and type information comes from the parser's spans and the checker.

mod associated;
mod display;
mod documentation;
#[cfg(test)]
mod extraction;
mod traits;

use std::path::{Path, PathBuf};

use crate::ast::{Block, Expr, ExprKind, Function, Module, Stmt};
use crate::check::{self, BindingFact, ExprType, NameUse, ProgramInfo};
use crate::error::CompileError;
use crate::lexer;
use crate::parser;

#[derive(Clone)]
pub struct Symbol {
    pub name: String,
    pub kind: SymbolKind,
    pub span: crate::ast::Span,
    pub detail: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SymbolKind {
    Function,
    Method,
    Struct,
    Trait,
    AssociatedType,
    Enum,
    Const,
    Field,
}

pub struct Analysis {
    pub source: String,
    pub path: Option<PathBuf>,
    pub diagnostics: Vec<CompileError>,
    pub info: ProgramInfo,
    pub symbols: Vec<Symbol>,
    ast: Module,
    documentation: std::collections::HashMap<usize, documentation::DocComment>,
}

impl Analysis {
    pub(crate) fn import_fixes(
        &self,
        start: usize,
        end: usize,
    ) -> Vec<crate::auto_import::ImportFix> {
        crate::auto_import::fixes(&self.source, &self.ast, &self.diagnostics, start, end)
    }

    pub fn open(path: Option<&Path>, source: &str) -> Self {
        let (parsed, parse_errors) = match lexer::tokenize(source) {
            Ok(tokens) => {
                let parsed = parser::parse_for_ide(source, &tokens);
                if let Some(repaired) =
                    crate::macros::editor::recover_parsed(source, &tokens, &parsed.1)
                {
                    let mut analysis = Self::open(path, &repaired);
                    analysis.source = source.to_owned();
                    analysis.diagnostics.splice(0..0, parsed.1);
                    return analysis;
                }
                parsed
            }
            Err(err) => {
                if let Some(repaired) = crate::macros::editor::recover(source, &err) {
                    let mut analysis = Self::open(path, &repaired);
                    analysis.source = source.to_owned();
                    analysis.diagnostics.insert(0, err);
                    return analysis;
                }
                return Self {
                    source: source.to_string(),
                    path: path.map(|path| path.to_path_buf()),
                    diagnostics: vec![err],
                    info: empty_info(),
                    symbols: Vec::new(),
                    ast: empty_module(),
                    documentation: std::collections::HashMap::new(),
                };
            }
        };
        let symbols = symbols_of(&parsed);
        let documentation = documentation::collect(source, &parsed);
        let (info, mut diagnostics) = if (!parsed.imports.is_empty()
            || !parsed.macro_imports.is_empty()
            || !parsed.item_macros.is_empty())
            && path.is_some()
        {
            match crate::modules::check_root(path.unwrap(), source, parsed.clone()) {
                Ok(checked) => checked,
                Err(err) => {
                    // Local facts remain useful if an import fails, but checking
                    // without its bindings invents unknown-name diagnostics.
                    let (info, _) = check::analyze(source, &parsed);
                    (info, vec![err])
                }
            }
        } else {
            let (info, result) = check::analyze(source, &parsed);
            (info, result.err().into_iter().collect())
        };
        if parse_errors
            .iter()
            .any(|error| error.detail == parser::EXPECTED_MEMBER_NAME)
        {
            // The parser creates an empty Field node to retain the receiver.
            // Its missing name is already reported at the dot; the checker
            // cannot meaningfully diagnose a field called the empty string.
            diagnostics.retain(|error| !error.detail.ends_with(" has no field ``"));
        }
        if parse_errors
            .iter()
            .any(|error| error.detail == parser::EXPECTED_ASSOCIATED_TYPE_NAME)
        {
            diagnostics.retain(|error| !error.detail.contains("associated type ``"));
        }
        diagnostics.splice(0..0, parse_errors);
        Self {
            source: source.to_string(),
            path: path.map(|path| path.to_path_buf()),
            diagnostics,
            info,
            symbols,
            ast: parsed,
            documentation,
        }
    }

    pub fn hover(&self, offset: usize) -> Option<String> {
        self.hover_type(offset).map(|text| self.display_type(&text))
    }

    fn hover_type(&self, offset: usize) -> Option<String> {
        if let Some(name) = self
            .uses()
            .find(|name| name.span.contains(offset) && !name.ty.is_empty())
        {
            return Some(name.ty.clone());
        }
        if let Some(fact) = self.facts().find(|fact| fact.def.contains(offset)) {
            return Some(format!("{}: {}", fact.name, fact.ty));
        }
        let expr = self.expr_at(offset)?;
        if let Some(sig) = self.info.call_sigs.iter().find(|sig| sig.span == expr.span) {
            return Some(sig.label.clone());
        }
        Some(expr.ty.clone())
    }

    pub fn definition(&self, offset: usize) -> Option<(Option<PathBuf>, crate::ast::Span)> {
        if let Some(name) = self.uses().find(|name| name.span.contains(offset)) {
            return Some((
                name.def_file.clone().or_else(|| self.path.clone()),
                name.def,
            ));
        }
        for import in self.ast.imports.iter().chain(&self.ast.macro_imports) {
            let name = import
                .names
                .iter()
                .find(|name| name.span.contains(offset) || name.alias_span.contains(offset));
            if import.path_span.contains(offset) || name.is_some() {
                let (path, span) = crate::modules::import_definition(
                    self.path.as_deref()?,
                    &import.path,
                    name.map(|name| name.name.as_str()),
                )?;
                return Some((Some(path), span));
            }
        }
        let identifier = ident_around(&self.source, offset);
        if self
            .source
            .get(identifier.end..)
            .is_some_and(|s| s.trim_start().starts_with('!'))
        {
            let name = &self.source[identifier.start..identifier.end];
            for import in &self.ast.macro_imports {
                if let Some(item) = import.names.iter().find(|n| n.alias == name) {
                    let (path, span) = crate::modules::import_definition(
                        self.path.as_deref()?,
                        &import.path,
                        Some(&item.name),
                    )?;
                    return Some((Some(path), span));
                }
            }
        }
        if let Some(name) = self.uses().find(|name| name.span.contains(offset)) {
            let path = name.def_file.clone().or_else(|| self.path.clone());
            return Some((path, name.def));
        }
        if let Some(fact) = self.facts().find(|fact| fact.def.contains(offset)) {
            return Some((self.path.clone(), fact.def));
        }
        self.field_definition(offset)
    }

    pub fn references(&self, offset: usize) -> Vec<SourceLoc> {
        let (def_file, def) =
            if let Some(name) = self.uses().find(|name| name.span.contains(offset)) {
                (name.def_file.clone(), name.def)
            } else if let Some(fact) = self.facts().find(|fact| fact.def.contains(offset)) {
                (None, fact.def)
            } else if let Some((span, _, _)) = self.field_at(offset) {
                return vec![SourceLoc {
                    path: self.path.clone(),
                    span,
                }];
            } else {
                return Vec::new();
            };
        let mut locs = vec![SourceLoc {
            path: def_file.clone().or_else(|| self.path.clone()),
            span: def,
        }];
        locs.extend(
            self.uses()
                .filter(|name| name.def == def && name.def_file == def_file)
                .map(|name| SourceLoc {
                    path: self.path.clone(),
                    span: name.span,
                }),
        );
        let mut seen = std::collections::HashSet::new();
        locs.retain(|loc| seen.insert((loc.path.clone(), loc.span.start, loc.span.end)));
        locs
    }

    pub fn completions(&self, offset: usize) -> Vec<Completion> {
        self.completion_items(offset)
            .into_iter()
            .map(|mut item| {
                item.detail = self.display_type(&item.detail);
                item
            })
            .collect()
    }

    fn completion_items(&self, offset: usize) -> Vec<Completion> {
        let replace = ident_around(&self.source, offset);
        if let Some(items) = self.trait_qualified_completions(replace) {
            return items;
        }
        if let Some(dot) = self.dot_before(offset) {
            return self.method_completions(dot, replace);
        }
        let mut items = Vec::new();
        let mut seen = Vec::new();
        for fact in self.visible(offset) {
            if seen.contains(&fact.name) {
                continue;
            }
            seen.push(fact.name.clone());
            let kind = if fact.variant {
                CompletionKind::EnumMember
            } else if fact.ty.starts_with("fn(") {
                CompletionKind::Function
            } else if fact.ty.starts_with("trait ") {
                CompletionKind::Trait
            } else if fact.ty.starts_with("type ") {
                CompletionKind::Type
            } else if fact.ty.starts_with("const ") {
                CompletionKind::Constant
            } else {
                CompletionKind::Variable
            };
            items.push(Completion {
                label: fact.name.clone(),
                kind,
                detail: fact.ty.clone(),
                replace,
                import_edit: None,
            });
        }
        for fact in self.facts().filter(|fact| {
            fact.scope.contains(offset)
                && offset >= fact.visible_from
                && (fact.ty.starts_with("fn(") || fact.variant)
                && !self
                    .ast
                    .impls
                    .iter()
                    .any(|item| item.items.iter().any(|method| method.name_span == fact.def))
        }) {
            if seen.contains(&fact.name) {
                continue;
            }
            seen.push(fact.name.clone());
            let kind = if fact.variant {
                CompletionKind::EnumMember
            } else {
                CompletionKind::Function
            };
            items.push(Completion {
                label: fact.name.clone(),
                kind,
                detail: fact.ty.clone(),
                replace,
                import_edit: None,
            });
        }
        for symbol in &self.symbols {
            if matches!(
                symbol.kind,
                SymbolKind::Field | SymbolKind::Method | SymbolKind::AssociatedType
            ) {
                continue;
            }
            if seen.contains(&symbol.name) {
                continue;
            }
            seen.push(symbol.name.clone());
            items.push(Completion {
                label: symbol.name.clone(),
                kind: match symbol.kind {
                    SymbolKind::Function | SymbolKind::Method => CompletionKind::Function,
                    SymbolKind::Struct | SymbolKind::Enum => CompletionKind::Type,
                    SymbolKind::Trait => CompletionKind::Trait,
                    SymbolKind::AssociatedType => CompletionKind::AssociatedType,
                    SymbolKind::Const => CompletionKind::Constant,
                    SymbolKind::Field => CompletionKind::Field,
                },
                detail: symbol.detail.clone(),
                replace,
                import_edit: None,
            });
        }
        if !seen.iter().any(|name| name == "Printable") {
            items.push(Completion {
                label: "Printable".into(),
                kind: CompletionKind::Trait,
                detail: "trait Printable".into(),
                replace,
                import_edit: None,
            });
        }
        for (name, signature) in check::BUILTIN_FUNCTIONS {
            if seen.iter().any(|found| found == name) {
                continue;
            }
            seen.push((*name).to_string());
            items.push(Completion {
                label: (*name).to_string(),
                kind: CompletionKind::Function,
                detail: (*signature).to_string(),
                replace,
                import_edit: None,
            });
        }
        for import in &self.ast.macro_imports {
            for name in &import.names {
                items.push(Completion {
                    label: name.alias.clone(),
                    kind: CompletionKind::Function,
                    detail: "procedural macro (TokenStream) -> TokenStream".into(),
                    replace,
                    import_edit: None,
                });
            }
        }
        for word in KEYWORDS {
            if !seen.iter().any(|name| name == word) {
                items.push(Completion {
                    label: (*word).to_string(),
                    kind: CompletionKind::Keyword,
                    detail: String::new(),
                    replace,
                    import_edit: None,
                });
            }
        }
        items
    }

    pub fn signature(&self, offset: usize) -> Option<(String, u32)> {
        let call = self.find_call(offset, &self.function_bodies())?;
        let label = self
            .info
            .call_sigs
            .iter()
            .find(|sig| sig.span == call.span)
            .map(|sig| sig.label.clone())
            .or_else(|| {
                let name = self.source.get(call.callee.start..call.callee.end)?;
                self.facts()
                    .find(|fact| fact.name == name && fact.ty.starts_with("fn("))
                    .map(|fact| fact.ty.clone())
            })?;
        let active = call
            .args
            .iter()
            .position(|span| offset < span.end)
            .unwrap_or(call.args.len()) as u32;
        Some((self.display_type(&label), active))
    }

    pub fn token_type(&self, span: crate::ast::Span) -> u32 {
        if let Some(fact) = self.facts().find(|fact| fact.def == span) {
            if fact.ty.starts_with("trait ") || fact.ty.starts_with("type ") {
                return 2;
            }
            if fact.variant {
                return 8;
            }
            if fact.ty.starts_with("fn(") {
                return 1;
            }
            if fact.def.end <= fact.scope.start {
                return 3;
            }
            return 4;
        }
        if self.symbols.iter().any(|symbol| {
            matches!(
                symbol.kind,
                SymbolKind::Struct
                    | SymbolKind::Enum
                    | SymbolKind::Trait
                    | SymbolKind::AssociatedType
            ) && symbol.span == span
        }) {
            return 2;
        }
        if let Some(name) = self.uses().find(|name| name.span == span) {
            if name.ty.starts_with("type ") || name.ty.starts_with("trait ") {
                return 2;
            }
            if self
                .facts()
                .any(|fact| fact.def == name.def && fact.variant)
            {
                return 8;
            }
            if name.ty.starts_with("fn(") {
                return 1;
            }
            if self
                .facts()
                .any(|fact| fact.def == name.def && fact.def.end <= fact.scope.start)
            {
                return 3;
            }
            return 4;
        }
        4
    }

    pub fn inlay_hints(&self) -> Vec<(usize, String)> {
        let mut hints = Vec::new();
        for func in &self.ast.functions {
            hints_in_block(&func.body, &self.info.facts, &mut hints);
        }
        hints
            .into_iter()
            .map(|(offset, label)| (offset, self.display_type(&label)))
            .collect()
    }

    fn method_completions(&self, dot: usize, replace: crate::ast::Span) -> Vec<Completion> {
        // An unfinished member at a function's tail can prevent return inference
        // before any local type facts are recorded. Check the receiver without
        // that member, preserving source offsets and all imports.
        let mut member_end = replace.end;
        let mut receiver_end = dot;
        if let Ok(tokens) = lexer::tokenize(&self.source) {
            if let Some(last) = tokens
                .iter()
                .rev()
                .find(|token| token.span.end <= dot && !matches!(token.kind, lexer::TokenKind::Eof))
            {
                receiver_end = last.span.end;
            }
            let rest: Vec<_> = tokens
                .iter()
                .filter(|token| token.span.start >= replace.end)
                .collect();
            if rest
                .first()
                .is_some_and(|token| matches!(token.kind, lexer::TokenKind::LParen))
            {
                let (mut parens, mut braces) = (0, 0);
                for token in rest {
                    match token.kind {
                        lexer::TokenKind::LParen => parens += 1,
                        lexer::TokenKind::RParen => {
                            parens -= 1;
                            if parens == 0 {
                                member_end = token.span.end;
                                break;
                            }
                        }
                        lexer::TokenKind::LBrace => braces += 1,
                        lexer::TokenKind::RBrace if braces == 0 => {
                            member_end = token.span.start;
                            break;
                        }
                        lexer::TokenKind::RBrace => braces -= 1,
                        _ => {}
                    }
                }
            }
        }
        let mut repaired = self.source.clone();
        let mask: String = repaired[dot..member_end]
            .chars()
            .map(|ch| {
                if matches!(ch, '\r' | '\n') {
                    ch.to_string()
                } else {
                    " ".repeat(ch.len_utf8())
                }
            })
            .collect();
        repaired.replace_range(dot..member_end, &mask);
        let recovered = Analysis::open(self.path.as_deref(), &repaired);
        let info = &recovered.info;
        let bytes = self.source.as_bytes();
        let mut end = receiver_end;
        while end > 0 && bytes[end - 1].is_ascii_whitespace() {
            end -= 1;
        }
        let Some(expr) = info
            .expr_types
            .iter()
            .filter(|expr| expr.span.end == end)
            .min_by_key(|expr| expr.span.end - expr.span.start)
            .or_else(|| {
                self.info
                    .expr_types
                    .iter()
                    .filter(|expr| expr.span.end == end)
                    .min_by_key(|expr| expr.span.end - expr.span.start)
            })
        else {
            return Vec::new();
        };
        let mut items = Vec::new();
        let ty = expr.ty.clone();
        let referent = ty
            .strip_prefix("&mut ")
            .or_else(|| ty.strip_prefix('&'))
            .unwrap_or(&ty);
        if !ty.starts_with('&')
            && self
                .ast
                .functions
                .iter()
                .any(|func| func.is_async && func.body.span.contains(dot))
            && info
                .trait_methods
                .iter()
                .chain(&self.info.trait_methods)
                .any(|method| {
                    method.receiver == expr.span
                        && method.name == "poll"
                        && method.trait_name.rsplit('$').next() == Some("Future")
                })
        {
            items.push(Completion {
                label: "await".into(),
                kind: CompletionKind::Keyword,
                detail: "Suspend until this future is ready".into(),
                replace,
                import_edit: None,
            });
        }
        for (name, detail) in builtin_methods(referent) {
            items.push(Completion {
                label: name,
                kind: CompletionKind::Method,
                detail,
                replace,
                import_edit: None,
            });
        }
        for (type_name, method) in &info.methods {
            if referent == type_name || referent.starts_with(&format!("{type_name}<")) {
                items.push(Completion {
                    label: method.clone(),
                    kind: CompletionKind::Method,
                    detail: type_name.clone(),
                    replace,
                    import_edit: None,
                });
            }
        }
        for method in info
            .trait_methods
            .iter()
            .chain(&self.info.trait_methods)
            .filter(|method| method.receiver == expr.span && method.receiver_type == expr.ty)
        {
            if !items.iter().any(|item| item.label == method.name) {
                items.push(Completion {
                    label: method.name.clone(),
                    kind: CompletionKind::Method,
                    detail: method.signature.clone(),
                    replace,
                    import_edit: None,
                });
            }
        }
        for (module, function) in crate::modules::std_completion_functions() {
            let prelude =
                module == "std/io" && matches!(function.name.as_str(), "print" | "println");
            // These free extensions take T by value, with the core Printable
            // bound. A shared receiver method alone does not prove that bound.
            if prelude
                && (ty.starts_with('&')
                    || !info
                        .trait_methods
                        .iter()
                        .chain(&self.info.trait_methods)
                        .any(|m| {
                            m.receiver == expr.span
                                && m.receiver_type == expr.ty
                                && m.trait_name == "$std$Printable"
                        }))
            {
                continue;
            }
            let imported = self
                .ast
                .imports
                .iter()
                .filter(|import| import.path.strip_suffix(".rfl").unwrap_or(&import.path) == module)
                .flat_map(|import| import.names.iter())
                .find(|name| name.name == function.name);
            let label = imported.map_or_else(|| function.name.clone(), |name| name.alias.clone());
            if items.iter().any(|item| item.label == label) {
                continue;
            }
            let shadowed = info.facts.iter().any(|fact| {
                fact.name == label
                    && fact.scope.start > 0
                    && fact.scope.contains(dot)
                    && dot >= fact.visible_from
            });
            let conflicts = self
                .ast
                .functions
                .iter()
                .any(|function| function.name == label)
                || self.ast.consts.iter().any(|item| item.name == label)
                || imported.is_none()
                    && self
                        .ast
                        .imports
                        .iter()
                        .any(|import| import.names.iter().any(|name| name.alias == label));
            if shadowed || conflicts {
                continue;
            }
            if let Some(detail) = function.method_signature(&ty) {
                let import_edit = if imported.is_some() || prelude {
                    None
                } else {
                    crate::auto_import::completion_edit(
                        &self.source,
                        &self.ast,
                        &module,
                        &function.name,
                    )
                };
                if imported.is_none() && !prelude && import_edit.is_none() {
                    continue;
                }
                let detail = if import_edit.is_some() {
                    format!("{detail} — from {module}")
                } else {
                    detail
                };
                items.push(Completion {
                    label,
                    kind: CompletionKind::Method,
                    detail,
                    replace,
                    import_edit,
                });
            }
        }
        items
    }

    /// The `.` of a member access, including when the cursor is in the partial name (`tape.l`).
    fn dot_before(&self, offset: usize) -> Option<usize> {
        let bytes = self.source.as_bytes();
        let mut index = offset.min(bytes.len());
        while index > 0 && is_ident_byte(bytes[index - 1]) {
            index -= 1;
        }
        while index > 0 && bytes[index - 1].is_ascii_whitespace() {
            index -= 1;
        }
        if index > 0 && bytes[index - 1] == b'.' {
            Some(index - 1)
        } else {
            None
        }
    }

    fn visible(&self, offset: usize) -> Vec<&BindingFact> {
        let mut found: Vec<&BindingFact> = self
            .facts()
            .filter(|fact| {
                fact.scope.contains(offset)
                    && offset >= fact.visible_from
                    && !fact.ty.starts_with("fn(")
            })
            .collect();
        found.sort_by_key(|fact| fact.scope.end.saturating_sub(fact.scope.start));
        found
    }

    fn field_definition(&self, offset: usize) -> Option<(Option<PathBuf>, crate::ast::Span)> {
        let (span, _, _) = self.field_at(offset)?;
        Some((self.path.clone(), span))
    }

    fn field_at(&self, offset: usize) -> Option<(crate::ast::Span, String, String)> {
        for func in &self.ast.functions {
            if let Some(found) = field_in_block(&func.body, offset, &self.info.expr_types) {
                if let Some(symbol) = self.symbols.iter().find(|symbol| {
                    symbol.kind == SymbolKind::Field
                        && symbol.name == found.1
                        && symbol.detail == found.2
                }) {
                    return Some((symbol.span, found.1, found.2));
                }
            }
        }
        None
    }

    fn expr_at(&self, offset: usize) -> Option<&ExprType> {
        self.info
            .expr_types
            .iter()
            .filter(|expr| expr.span.contains(offset))
            .min_by_key(|expr| expr.span.end - expr.span.start)
    }

    fn facts(&self) -> impl Iterator<Item = &BindingFact> {
        self.info.facts.iter()
    }

    fn uses(&self) -> impl Iterator<Item = &NameUse> {
        self.info
            .uses
            .iter()
            .filter(|name| name.span.end <= self.source.len())
    }

    fn function_bodies(&self) -> Vec<&Block> {
        self.ast
            .functions
            .iter()
            .map(|func| &func.body)
            .chain(
                self.ast
                    .impls
                    .iter()
                    .flat_map(|item| item.items.iter().map(|m| &m.body)),
            )
            .collect()
    }

    fn find_call(&self, offset: usize, blocks: &[&Block]) -> Option<CallAt> {
        for block in blocks {
            if !block.span.contains(offset) && offset != block.span.end {
                continue;
            }
            for stmt in &block.stmts {
                if let Some(found) = call_in_stmt(stmt, offset) {
                    return Some(found);
                }
            }
            if let Some(expr) = &block.tail {
                if let Some(found) = call_in_expr(expr, offset) {
                    return Some(found);
                }
            }
        }
        None
    }
}

struct CallAt {
    span: crate::ast::Span,
    callee: crate::ast::Span,
    args: Vec<crate::ast::Span>,
}

fn field_in_block(
    block: &Block,
    offset: usize,
    types: &[ExprType],
) -> Option<(crate::ast::Span, String, String)> {
    for stmt in &block.stmts {
        if let Some(found) = field_in_stmt(stmt, offset, types) {
            return Some(found);
        }
    }
    block
        .tail
        .as_ref()
        .and_then(|expr| field_in_expr(expr, offset, types))
}

fn field_in_stmt(
    stmt: &Stmt,
    offset: usize,
    types: &[ExprType],
) -> Option<(crate::ast::Span, String, String)> {
    match stmt {
        Stmt::Let { init, .. } => field_in_expr(init, offset, types),
        Stmt::Assign { value, .. }
        | Stmt::DerefAssign { value, .. }
        | Stmt::FieldAssign { value, .. } => field_in_expr(value, offset, types),
        Stmt::IndexAssign { index, value, .. } => {
            field_in_expr(index, offset, types).or_else(|| field_in_expr(value, offset, types))
        }
        Stmt::Expr(expr)
        | Stmt::Return {
            value: Some(expr), ..
        } => field_in_expr(expr, offset, types),
        _ => None,
    }
}

fn field_in_expr(
    expr: &Expr,
    offset: usize,
    types: &[ExprType],
) -> Option<(crate::ast::Span, String, String)> {
    if !expr.span.contains(offset) {
        return None;
    }
    match &expr.kind {
        ExprKind::Field {
            base,
            name,
            name_span,
        } if name_span.contains(offset) => {
            let ty = types
                .iter()
                .find(|expr| expr.span == base.span)
                .map(|expr| expr.ty.clone())
                .unwrap_or_default();
            Some((*name_span, name.clone(), ty))
        }
        ExprKind::Field { base, .. } => field_in_expr(base, offset, types),
        ExprKind::Call { args, .. }
        | ExprKind::Array(args)
        | ExprKind::Tuple(args)
        | ExprKind::Assoc { args, .. } => args
            .iter()
            .find_map(|arg| field_in_expr(arg, offset, types)),
        ExprKind::StructLit { fields, .. } => fields
            .iter()
            .find_map(|(_, expr)| field_in_expr(expr, offset, types)),
        ExprKind::Lambda { body, .. } => field_in_expr(body, offset, types),
        ExprKind::Method { receiver, args, .. } => {
            field_in_expr(receiver, offset, types).or_else(|| {
                args.iter()
                    .find_map(|arg| field_in_expr(arg, offset, types))
            })
        }
        ExprKind::Binary { lhs, rhs, .. } => {
            field_in_expr(lhs, offset, types).or_else(|| field_in_expr(rhs, offset, types))
        }
        ExprKind::Unary { expr, .. }
        | ExprKind::Await(expr)
        | ExprKind::Try(expr)
        | ExprKind::Deref(expr)
        | ExprKind::Borrow { expr, .. } => field_in_expr(expr, offset, types),
        ExprKind::Index { base, index } => {
            field_in_expr(base, offset, types).or_else(|| field_in_expr(index, offset, types))
        }
        ExprKind::If {
            cond,
            then_body,
            else_body,
        } => field_in_expr(cond, offset, types)
            .or_else(|| field_in_block(then_body, offset, types))
            .or_else(|| {
                else_body
                    .as_ref()
                    .and_then(|expr| field_in_expr(expr, offset, types))
            }),
        ExprKind::Block(block) | ExprKind::Unsafe(block) => field_in_block(block, offset, types),
        ExprKind::While { cond, body } => {
            field_in_expr(cond, offset, types).or_else(|| field_in_block(body, offset, types))
        }
        ExprKind::For { iter, body, .. } => {
            field_in_expr(iter, offset, types).or_else(|| field_in_block(body, offset, types))
        }
        ExprKind::Match { scrutinee, arms } => {
            field_in_expr(scrutinee, offset, types).or_else(|| {
                arms.iter().find_map(|arm| {
                    arm.guard
                        .as_ref()
                        .and_then(|guard| field_in_expr(guard, offset, types))
                        .or_else(|| field_in_expr(&arm.body, offset, types))
                })
            })
        }
        _ => None,
    }
}

fn call_in_stmt(stmt: &Stmt, offset: usize) -> Option<CallAt> {
    match stmt {
        Stmt::Let { init, .. } => call_in_expr(init, offset),
        Stmt::Assign { value, .. }
        | Stmt::DerefAssign { value, .. }
        | Stmt::FieldAssign { value, .. } => call_in_expr(value, offset),
        Stmt::IndexAssign { index, value, .. } => {
            call_in_expr(index, offset).or_else(|| call_in_expr(value, offset))
        }
        Stmt::Expr(expr)
        | Stmt::Return {
            value: Some(expr), ..
        } => call_in_expr(expr, offset),
        Stmt::Return { value: None, .. } | Stmt::Break { .. } | Stmt::Continue { .. } => None,
    }
}

fn call_in_expr(expr: &Expr, offset: usize) -> Option<CallAt> {
    if !expr.span.contains(offset) {
        return None;
    }
    match &expr.kind {
        ExprKind::Call {
            callee_span, args, ..
        } if callee_span.end <= offset => {
            for arg in args {
                if let Some(found) = call_in_expr(arg, offset) {
                    return Some(found);
                }
            }
            Some(CallAt {
                span: expr.span,
                callee: *callee_span,
                args: args.iter().map(|arg| arg.span).collect(),
            })
        }
        ExprKind::Assoc { args, .. } => {
            for arg in args {
                if let Some(found) = call_in_expr(arg, offset) {
                    return Some(found);
                }
            }
            Some(CallAt {
                span: expr.span,
                callee: crate::ast::Span::new(expr.span.start, expr.span.start),
                args: args.iter().map(|arg| arg.span).collect(),
            })
        }
        ExprKind::Method {
            receiver,
            method_span,
            args,
            ..
        } => {
            if let Some(found) = call_in_expr(receiver, offset) {
                return Some(found);
            }
            for arg in args {
                if let Some(found) = call_in_expr(arg, offset) {
                    return Some(found);
                }
            }
            if offset >= method_span.start {
                Some(CallAt {
                    span: expr.span,
                    callee: *method_span,
                    args: args.iter().map(|arg| arg.span).collect(),
                })
            } else {
                None
            }
        }
        ExprKind::Binary { lhs, rhs, .. } => {
            call_in_expr(lhs, offset).or_else(|| call_in_expr(rhs, offset))
        }
        ExprKind::Unary { expr, .. }
        | ExprKind::Await(expr)
        | ExprKind::Try(expr)
        | ExprKind::Borrow { expr, .. }
        | ExprKind::Deref(expr) => call_in_expr(expr, offset),
        ExprKind::If {
            cond,
            then_body,
            else_body,
        } => call_in_expr(cond, offset)
            .or_else(|| call_in_block(then_body, offset))
            .or_else(|| {
                else_body
                    .as_ref()
                    .and_then(|expr| call_in_expr(expr, offset))
            }),
        ExprKind::Block(block) | ExprKind::Unsafe(block) => call_in_block(block, offset),
        ExprKind::While { cond, body } => {
            call_in_expr(cond, offset).or_else(|| call_in_block(body, offset))
        }
        ExprKind::For { iter, body, .. } => {
            call_in_expr(iter, offset).or_else(|| call_in_block(body, offset))
        }
        ExprKind::Match { scrutinee, arms } => call_in_expr(scrutinee, offset).or_else(|| {
            arms.iter().find_map(|arm| {
                arm.guard
                    .as_ref()
                    .and_then(|guard| call_in_expr(guard, offset))
                    .or_else(|| call_in_expr(&arm.body, offset))
            })
        }),
        ExprKind::Index { base, index } => {
            call_in_expr(base, offset).or_else(|| call_in_expr(index, offset))
        }
        ExprKind::Field { base, .. } => call_in_expr(base, offset),
        ExprKind::Array(elems) | ExprKind::Tuple(elems) => {
            elems.iter().find_map(|elem| call_in_expr(elem, offset))
        }
        _ => None,
    }
}

fn call_in_block(block: &Block, offset: usize) -> Option<CallAt> {
    if !block.span.contains(offset) {
        return None;
    }
    for stmt in &block.stmts {
        if let Some(found) = call_in_stmt(stmt, offset) {
            return Some(found);
        }
    }
    block
        .tail
        .as_ref()
        .and_then(|expr| call_in_expr(expr, offset))
}

fn hints_in_block(block: &Block, facts: &[BindingFact], hints: &mut Vec<(usize, String)>) {
    for stmt in &block.stmts {
        if let Stmt::Let {
            pattern, ty: None, ..
        } = stmt
        {
            if let crate::ast::Pat::Name {
                name,
                span: name_span,
            } = pattern
            {
                if name != "_" {
                    if let Some(fact) = facts.iter().find(|fact| fact.def == *name_span) {
                        if !fact.ty.starts_with("fn(") {
                            hints.push((name_span.end, format!(": {}", fact.ty)));
                        }
                    }
                }
            }
        }
        walk_stmt_hints(stmt, facts, hints);
    }
    if let Some(expr) = &block.tail {
        hints_in_expr(expr, facts, hints);
    }
}

fn walk_stmt_hints(stmt: &Stmt, facts: &[BindingFact], hints: &mut Vec<(usize, String)>) {
    match stmt {
        Stmt::Let { init, .. } => hints_in_expr(init, facts, hints),
        Stmt::Assign { value, .. }
        | Stmt::DerefAssign { value, .. }
        | Stmt::FieldAssign { value, .. } => hints_in_expr(value, facts, hints),
        Stmt::IndexAssign { index, value, .. } => {
            hints_in_expr(index, facts, hints);
            hints_in_expr(value, facts, hints);
        }
        Stmt::Expr(expr)
        | Stmt::Return {
            value: Some(expr), ..
        } => hints_in_expr(expr, facts, hints),
        _ => {}
    }
}

fn hints_in_expr(expr: &Expr, facts: &[BindingFact], hints: &mut Vec<(usize, String)>) {
    match &expr.kind {
        ExprKind::Block(block) | ExprKind::Unsafe(block) => hints_in_block(block, facts, hints),
        ExprKind::If {
            then_body,
            else_body,
            cond,
        } => {
            hints_in_expr(cond, facts, hints);
            hints_in_block(then_body, facts, hints);
            if let Some(expr) = else_body {
                hints_in_expr(expr, facts, hints);
            }
        }
        ExprKind::While { cond, body } => {
            hints_in_expr(cond, facts, hints);
            hints_in_block(body, facts, hints);
        }
        ExprKind::For { iter, body, .. } => {
            hints_in_expr(iter, facts, hints);
            hints_in_block(body, facts, hints);
        }
        ExprKind::Match { scrutinee, arms } => {
            hints_in_expr(scrutinee, facts, hints);
            for arm in arms {
                if let Some(guard) = &arm.guard {
                    hints_in_expr(guard, facts, hints);
                }
                hints_in_expr(&arm.body, facts, hints);
            }
        }
        ExprKind::Call { args, .. } | ExprKind::Array(args) | ExprKind::Tuple(args) => {
            for arg in args {
                hints_in_expr(arg, facts, hints);
            }
        }
        ExprKind::Method { receiver, args, .. } => {
            hints_in_expr(receiver, facts, hints);
            for arg in args {
                hints_in_expr(arg, facts, hints);
            }
        }
        ExprKind::Binary { lhs, rhs, .. } => {
            hints_in_expr(lhs, facts, hints);
            hints_in_expr(rhs, facts, hints);
        }
        ExprKind::Unary { expr, .. }
        | ExprKind::Await(expr)
        | ExprKind::Try(expr)
        | ExprKind::Deref(expr)
        | ExprKind::Borrow { expr, .. }
        | ExprKind::Field { base: expr, .. } => {
            hints_in_expr(expr, facts, hints);
        }
        ExprKind::Index { base, index } => {
            hints_in_expr(base, facts, hints);
            hints_in_expr(index, facts, hints);
        }
        _ => {}
    }
}

pub struct SourceLoc {
    pub path: Option<PathBuf>,
    pub span: crate::ast::Span,
}

pub struct Completion {
    pub label: String,
    pub kind: CompletionKind,
    pub detail: String,
    /// The identifier being typed. Empty when the cursor is directly after `.`.
    pub replace: crate::ast::Span,
    /// An import to apply atomically with selection of an out-of-scope stdlib method.
    pub import_edit: Option<(crate::ast::Span, String)>,
}

fn ident_around(source: &str, offset: usize) -> crate::ast::Span {
    let bytes = source.as_bytes();
    let mut start = offset.min(bytes.len());
    while start > 0 && is_ident_byte(bytes[start - 1]) {
        start -= 1;
    }
    let mut end = offset.min(bytes.len());
    while end < bytes.len() && is_ident_byte(bytes[end]) {
        end += 1;
    }
    crate::ast::Span::new(start, end)
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

/// `Vec<u16>` and `[u16; 4]` carry their element type in the checker's display.
fn builtin_methods(ty: &str) -> Vec<(String, String)> {
    let elem = type_argument(ty);
    let mut methods = Vec::new();
    let mut add = |name: &str, detail: String| methods.push((name.to_string(), detail));
    if ty.starts_with("Vec<") {
        add("len", "fn() -> usize".to_string());
        add("is_empty", "fn() -> bool".to_string());
        add("push", format!("fn({elem}) -> ()"));
        add("pop", format!("fn() -> Option<{elem}>"));
        add("get", format!("fn(usize) -> Option<{elem}>"));
        add("clear", "fn() -> ()".to_string());
    } else if ty.starts_with("Owned<") {
        add("borrow", format!("fn() -> &{elem}"));
        add("borrow_mut", format!("fn() -> &mut {elem}"));
        add("into_inner", format!("fn() -> {elem}"));
    } else if ty == "String" {
        add("len", "fn() -> usize".to_string());
        add("is_empty", "fn() -> bool".to_string());
    } else if ty.starts_with("Slice<") || ty.starts_with('[') {
        add("len", "fn() -> usize".to_string());
        add("is_empty", "fn() -> bool".to_string());
        add("get", format!("fn(usize) -> Option<{elem}>"));
        add("to_vec", format!("fn() -> Vec<{elem}>"));
    }
    methods
}

fn type_argument(ty: &str) -> String {
    if let Some(rest) = ty
        .strip_prefix("Vec<")
        .or_else(|| ty.strip_prefix("Slice<"))
        .or_else(|| ty.strip_prefix("Owned<"))
    {
        if let Some(elem) = rest.strip_suffix('>') {
            return elem.to_string();
        }
    }
    if let Some(rest) = ty.strip_prefix('[') {
        if let Some(end) = rest.find(';') {
            return rest[..end].trim().to_string();
        }
    }
    "_".to_string()
}

#[derive(Clone, Copy)]
pub enum CompletionKind {
    Keyword,
    Function,
    Method,
    Variable,
    Type,
    Trait,
    AssociatedType,
    Constant,
    Field,
    EnumMember,
}

const KEYWORDS: &[&str] = &[
    "async", "await", "fn", "let", "mut", "if", "else", "return", "as", "while", "break",
    "continue", "match", "for", "in", "type", "trait", "impl", "const", "struct", "export",
    "import", "extern", "unsafe", "true", "false",
];

fn symbols_of(module: &Module) -> Vec<Symbol> {
    let mut symbols = Vec::new();
    for func in &module.functions {
        push_function(&mut symbols, func, SymbolKind::Function);
    }
    for item in &module.impls {
        for associated in &item.associated_types {
            symbols.push(Symbol {
                name: associated.name.clone(),
                kind: SymbolKind::AssociatedType,
                span: associated.name_span,
                detail: associated
                    .value
                    .as_ref()
                    .map(check::completion_type_name)
                    .unwrap_or_default(),
            });
        }
        for method in &item.items {
            symbols.push(Symbol {
                name: method.name.clone(),
                kind: SymbolKind::Method,
                span: crate::ast::Span::new(method.name_span.start, method.body.span.end),
                detail: String::new(),
            });
        }
    }
    for tr in &module.traits {
        for associated in &tr.associated_types {
            symbols.push(Symbol {
                name: associated.name.clone(),
                kind: SymbolKind::AssociatedType,
                span: associated.name_span,
                detail: format!("{}::{}", tr.name, associated.name),
            });
        }
        symbols.push(Symbol {
            name: tr.name.clone(),
            kind: SymbolKind::Trait,
            span: tr.name_span,
            detail: format!("trait {}", tr.name),
        });
        for method in &tr.methods {
            symbols.push(Symbol {
                name: method.name.clone(),
                kind: SymbolKind::Method,
                span: method.name_span,
                detail: tr.name.clone(),
            });
        }
    }
    for adt in &module.adts {
        symbols.push(Symbol {
            name: adt.name.clone(),
            kind: SymbolKind::Enum,
            span: adt.name_span,
            detail: String::new(),
        });
    }
    for item in &module.structs {
        symbols.push(Symbol {
            name: item.name.clone(),
            kind: SymbolKind::Struct,
            span: item.name_span,
            detail: String::new(),
        });
        for field in &item.fields {
            symbols.push(Symbol {
                name: field.name.clone(),
                kind: SymbolKind::Field,
                span: field.name_span,
                detail: item.name.clone(),
            });
        }
    }
    for item in &module.consts {
        symbols.push(Symbol {
            name: item.name.clone(),
            kind: SymbolKind::Const,
            span: item.name_span,
            detail: String::new(),
        });
    }
    symbols
}

fn push_function(symbols: &mut Vec<Symbol>, func: &Function, kind: SymbolKind) {
    symbols.push(Symbol {
        name: func.name.clone(),
        kind,
        span: crate::ast::Span::new(
            func.name_span.start,
            func.body.span.end.max(func.name_span.end),
        ),
        detail: String::new(),
    });
}

fn empty_info() -> ProgramInfo {
    ProgramInfo {
        display_names: std::collections::HashMap::new(),
        facts: Vec::new(),
        uses: Vec::new(),
        expr_types: Vec::new(),
        methods: Vec::new(),
        call_sigs: Vec::new(),
        trait_methods: Vec::new(),
        type_members: Vec::new(),
    }
}

fn empty_module() -> Module {
    Module {
        adts: Vec::new(),
        structs: Vec::new(),
        impls: Vec::new(),
        traits: Vec::new(),
        trait_scopes: Vec::new(),
        consts: Vec::new(),
        functions: Vec::new(),
        externs: Vec::new(),
        imports: Vec::new(),
        macro_imports: Vec::new(),
        macro_exports: Vec::new(),
        item_macros: Vec::new(),
        proc_macros: Vec::new(),
        macro_expansion: None,
    }
}

#[cfg(test)]
mod tests {
    use super::Analysis;

    #[test]
    fn builtins_complete_without_imports_and_use_the_checked_signatures() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("examples/builtin-completion.rfl");
        for imports in ["", "import { sin, PI } from \"std/math\";\n"] {
            let source = format!("{imports}fn main() {{ pri(1); }}");
            let at = source.find("pri(").unwrap() + 3;
            let doc = Analysis::open(Some(&path), &source);
            let items = doc.completions(at);
            for (name, signature) in crate::check::BUILTIN_FUNCTIONS {
                let matching: Vec<_> = items.iter().filter(|item| item.label == *name).collect();
                assert_eq!(matching.len(), 1, "{name}");
                let item = matching[0];
                assert!(matches!(item.kind, super::CompletionKind::Function));
                assert_eq!(&item.detail, signature);
                assert!(item.import_edit.is_none());
                assert_eq!(&source[item.replace.start..item.replace.end], "pri");
            }
            let item = items.iter().find(|item| item.label == "println").unwrap();
            let mut edited = source.clone();
            edited.replace_range(item.replace.start..item.replace.end, &item.label);
            let after = Analysis::open(Some(&path), &edited);
            assert!(after.diagnostics.is_empty(), "{:?}", after.diagnostics);
        }
        let source = "fn main() { print(1); println(\"hi\"); putc('a'); let a = from_char('a'); let b = from_byte(65 as u8); let owner=owned(1,fn(x:&mut i16)=>{}); let other=move(owner); drop(move(other)); }";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        for (name, signature) in crate::check::BUILTIN_FUNCTIONS {
            let at = source.find(&format!("{name}(")).unwrap() + name.len() + 1;
            let expected = match *name {
                "print" => "fn(i16) -> unit",
                "println" => "fn(String) -> unit",
                "owned" => "fn(i16, fn(&mut i16) -> unit) -> Owned<i16>",
                "move" => "fn(Owned<i16>) -> Owned<i16>",
                "drop" => "fn(Owned<i16>) -> unit",
                _ => signature,
            };
            assert_eq!(doc.signature(at).unwrap().0, expected);
        }
        let source = "fn main() { let text = \"hi\"; text. }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.find("text.").unwrap() + 5);
        for (name, _) in crate::check::BUILTIN_FUNCTIONS {
            if matches!(*name, "print" | "println") {
                continue;
            } // Printable/extension methods.
            assert!(!items.iter().any(|item| item.label == *name), "{name}");
        }
    }

    #[test]
    fn imports_navigate_to_exported_declarations_and_module_files() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
        let path = root.join("examples/import-navigation.rfl");
        // Unrelated broken imports and unfinished expressions must not prevent navigation.
        let source = "import { missing } from \"./does-not-exist\";\nimport { sin as sine, PI as pi } from \"std/math.rfl\";\nimport { List as Items, Cons } from \"std/list\";\nimport { Some } from \"std/option\";\nimport { replace } from \"std/string\";\nfn main() { missing. }";
        let doc = Analysis::open(Some(&path), source);
        for (text, module, name) in [
            ("sin as", "math", "sin"),
            ("sine,", "math", "sin"),
            ("PI as", "math", "PI"),
            ("pi }", "math", "PI"),
            ("List as", "list", "List"),
            ("Items,", "list", "List"),
            ("Cons }", "list", "Cons"),
            ("replace }", "string", "replace"),
        ] {
            let (file, span) = doc.definition(source.find(text).unwrap() + 1).unwrap();
            let file = file.unwrap();
            assert_eq!(
                file,
                root.join(format!("std/{module}.rfl"))
                    .canonicalize()
                    .unwrap()
            );
            let target = std::fs::read_to_string(file).unwrap();
            assert_eq!(&target[span.start..span.end], name);
        }
        for (text, module) in [
            ("std/math.rfl", "math"),
            ("std/list", "list"),
            ("Some }", "option"),
        ] {
            let (file, span) = doc.definition(source.find(text).unwrap() + 1).unwrap();
            assert_eq!(
                file.unwrap(),
                root.join(format!("std/{module}.rfl"))
                    .canonicalize()
                    .unwrap()
            );
            assert_eq!(span, crate::ast::Span::empty());
        }
        assert!(doc.definition(source.find("missing }").unwrap()).is_none());
        let invalid = "import { not_exported } from \"std/math\";\nfn main() {}";
        let doc = Analysis::open(Some(&path), invalid);
        assert!(doc
            .definition(invalid.find("not_exported").unwrap())
            .is_none());
        assert!(doc.definition(invalid.find("std/math").unwrap()).is_some());
        assert!(doc.definition(invalid.find("from").unwrap()).is_none());
    }

    #[test]
    fn companion_library_imports_navigate_and_hover() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
        let path = root.join("examples/companion-navigation.rfl");
        let source = "import { parse } from \"lib/ini\"; fn main() { parse(\"x=y\"); }";
        let doc = Analysis::open(Some(&path), source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let (file, span) = doc.definition(source.find("parse }").unwrap()).unwrap();
        let file = file.unwrap();
        assert_eq!(file, root.join("lib/ini.rfl").canonicalize().unwrap());
        assert_eq!(
            &std::fs::read_to_string(file).unwrap()[span.start..span.end],
            "parse"
        );
        let hover = doc.hover_markdown(source.rfind("parse(").unwrap()).unwrap();
        assert!(hover.contains("shared C reader"), "{hover}");
    }

    const EDITING_SOURCE: &str = "import { replace } from \"std/string\";\nimport { sin, PI } from \"std/math\";\nfn main() {\n let sample_rate: u32 = 44100;\n let frequency = 440.0;\n for (i in 0..sample_rate) {\n  let t = (i as f64) / (sample_rate as f64);\n  let sample = sin(2.0 * PI * frequency * t);\n  println(sample * 0.5 + 0.5);\n }\n let a = \"something\";\n let b = a.replace(\"some\", \"oth\");\n println(b);\n b.\n}\n";

    #[test]
    fn unfinished_member_reports_only_the_dot_and_keeps_imports() {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/diagnostics-test.rfl");
        let doc = Analysis::open(Some(&path), EDITING_SOURCE);
        assert_eq!(doc.diagnostics.len(), 1, "{:?}", doc.diagnostics);
        assert_eq!(
            doc.diagnostics[0].detail,
            crate::parser::EXPECTED_MEMBER_NAME
        );
        let dot = EDITING_SOURCE.rfind("b.").unwrap() + 1;
        assert_eq!(doc.diagnostics[0].offset, Some(dot));
        assert!(doc
            .completions(dot + 1)
            .iter()
            .any(|item| item.label == "trim"));

        let missing_import = EDITING_SOURCE.replace("sin, ", "");
        let doc = Analysis::open(Some(&path), &missing_import);
        assert!(
            doc.diagnostics
                .iter()
                .any(|error| error.detail == "unknown function `sin`"),
            "{:?}",
            doc.diagnostics
        );
        assert!(doc
            .diagnostics
            .iter()
            .any(|error| error.detail == crate::parser::EXPECTED_MEMBER_NAME));

        let failed_import =
            format!("import {{ missing }} from \"./does-not-exist\";\n{EDITING_SOURCE}");
        let doc = Analysis::open(Some(&path), &failed_import);
        assert_eq!(doc.diagnostics.len(), 2, "{:?}", doc.diagnostics);
        assert!(
            !doc.diagnostics
                .iter()
                .any(|error| error.detail.contains("unknown function")),
            "{:?}",
            doc.diagnostics
        );
    }

    #[test]
    fn ordinary_completion_shows_root_scope_and_imported_aliases_only() {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/completion-test.rfl");
        let source = EDITING_SOURCE.replace("b.\n", "freq\n");
        let at = source.rfind("freq\n").unwrap() + 4;
        let doc = Analysis::open(Some(&path), &source);
        let items = doc.completions(at);
        let labels: Vec<_> = items.iter().map(|item| item.label.as_str()).collect();
        for name in [
            "sin",
            "replace",
            "PI",
            "main",
            "a",
            "b",
            "sample_rate",
            "frequency",
        ] {
            assert!(labels.contains(&name), "missing {name}: {labels:?}");
        }
        for name in [
            "fract",
            "from",
            "region_eq",
            "__rcc_f64_reduce_pi2",
            "__rcc_f64_floor",
            "i",
            "t",
            "sample",
        ] {
            assert!(!labels.contains(&name), "unexpected {name}: {labels:?}");
        }
        assert!(!labels.iter().any(|name| name.contains('$')), "{labels:?}");
        let frequency = items.iter().find(|item| item.label == "frequency").unwrap();
        assert_eq!(
            &source[frequency.replace.start..frequency.replace.end],
            "freq"
        );
        assert_eq!(frequency.detail, "f64");
        assert!(
            items
                .iter()
                .any(|item| item.label == "PI"
                    && matches!(item.kind, super::CompletionKind::Constant))
        );

        let source = "import { sin as sine, PI as pi } from \"std/math\";\nimport { List as Items } from \"std/list\";\nfn main() { let value = sine(pi); value; }";
        let at = source.rfind("value;").unwrap();
        let doc = Analysis::open(Some(&path), source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
        let items = doc.completions(at);
        let labels: Vec<_> = items.iter().map(|item| item.label.as_str()).collect();
        for name in ["sine", "pi", "Items", "value", "main"] {
            assert!(labels.contains(&name), "missing {name}: {labels:?}");
        }
        for name in ["sin", "PI", "List", "from"] {
            assert!(!labels.contains(&name), "unexpected {name}: {labels:?}");
        }
        assert!(items
            .iter()
            .any(|item| item.label == "Items" && matches!(item.kind, super::CompletionKind::Type)));
        // Linked source offsets must preserve local navigation as well as scope.
        let (file, span) = doc.definition(at).unwrap();
        assert_eq!(file.as_deref(), Some(path.as_path()));
        assert_eq!(&source[span.start..span.end], "value");
    }

    #[test]
    fn loop_completion_keeps_bindings_inside_the_body() {
        for iter in ["0..3", "[1, 2, 3]"] {
            let source =
                format!("fn main() {{ for (item in {iter}) {{ println(item); }} println(0); }}");
            let doc = Analysis::open(None, &source);
            assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics);
            let inside = source.find("println(item)").unwrap();
            assert!(doc
                .completions(inside)
                .iter()
                .any(|item| item.label == "item"));
            let outside = source.find("println(0)").unwrap();
            assert!(!doc
                .completions(outside)
                .iter()
                .any(|item| item.label == "item"));
        }
    }

    #[test]
    fn imported_extensions_complete_after_inferred_string_and_partial_member() {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/completion-test.rfl");
        for member in ["", "rep"] {
            let source = format!("import {{ replace }} from \"std/string\";\nfn main() {{\n let a = \"something\";\n let b = a.replace(\"some\", \"oth\");\n println(b);\n b.{member}\n}}\n");
            let at = source.rfind("b.").unwrap() + 2 + member.len();
            let doc = Analysis::open(Some(&path), &source);
            let items = doc.completions(at);
            let labels: Vec<_> = items.iter().map(|item| item.label.as_str()).collect();
            assert!(
                labels.contains(&"replace"),
                "{member}: {labels:?}; {:?}",
                doc.diagnostics
            );
            assert!(
                labels.contains(&"len") && labels.contains(&"is_empty"),
                "{labels:?}"
            );
            assert!(
                !labels.contains(&"region_eq") && !labels.contains(&"reverse"),
                "{labels:?}"
            );
            let replace = items.iter().find(|item| item.label == "replace").unwrap();
            assert_eq!(replace.detail, "fn(String, String) -> String");
            assert_eq!(&source[replace.replace.start..replace.replace.end], member);
            assert!(!labels.contains(&"push"), "{labels:?}");
        }
    }

    #[test]
    fn extension_completion_preserves_aliases_and_filters_receiver_types() {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/completion-test.rfl");
        let source = "import { replace as substitute } from \"std/string\";\nimport { abs } from \"std/math\";\nfn main() { let text = \"x\"; text. }";
        let doc = Analysis::open(Some(&path), source);
        let labels: Vec<_> = doc
            .completions(source.rfind("text.").unwrap() + 5)
            .into_iter()
            .map(|item| item.label)
            .collect();
        assert!(
            labels.iter().any(|name| name == "substitute"),
            "{labels:?}; {:?}",
            doc.diagnostics
        );
        assert!(
            !labels
                .iter()
                .any(|name| matches!(name.as_str(), "replace" | "abs" | "region_eq")),
            "{labels:?}"
        );
    }

    #[test]
    fn extensions_are_stdlib_only_and_builtins_keep_priority() {
        let source = "fn identity<T>(value: T) -> T { value }\nfn size(value: String) -> usize { value.len() }\nfn number(value: i16) -> i16 { value }\nfn len(value: String) -> i16 { 1 }\nfn main() { let text = \"x\"; text. }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.rfind("text.").unwrap() + 5);
        assert!(!items
            .iter()
            .any(|item| matches!(item.label.as_str(), "identity" | "size" | "number")));
        assert!(items
            .iter()
            .any(|item| item.label == "trim" && item.import_edit.is_some()));
        assert_eq!(items.iter().filter(|item| item.label == "len").count(), 1);
        assert_eq!(
            items
                .iter()
                .find(|item| item.label == "len")
                .unwrap()
                .detail,
            "fn() -> usize"
        );
        let source = source.replace("let text", "let trim = 1; let text");
        let doc = Analysis::open(None, &source);
        assert!(!doc
            .completions(source.rfind("text.").unwrap() + 5)
            .iter()
            .any(|item| item.label == "trim"));
    }

    #[test]
    fn selecting_a_stdlib_method_completes_the_name_and_import() {
        let path =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/completion-test.rfl");
        let source = "// header 🦀\r\nimport { replace } from \"std/string\"; // keep\r\nfn main() { let b = \"x\".replace(\"x\", \"a\"); println(b); b.tr; }\r\n";
        let at = source.find("b.tr").unwrap() + 4;
        let doc = Analysis::open(Some(&path), source);
        let items = doc.completions(at);
        let completion = items.iter().find(|item| item.label == "trim").unwrap();
        assert!(completion.detail.contains("std/string"));
        let (import_span, import_text) = completion.import_edit.as_ref().unwrap();
        let mut edited = source.to_string();
        // Apply from the end of the document so earlier offsets remain valid.
        edited.replace_range(completion.replace.start..completion.replace.end, "trim()");
        edited.replace_range(import_span.start..import_span.end, import_text);
        assert!(edited.contains("import { trim, replace }"));
        assert!(edited.contains("; // keep\r\n"));
        let after = Analysis::open(Some(&path), &edited);
        assert!(
            after.diagnostics.is_empty(),
            "{edited}\n{:?}",
            after.diagnostics
        );
        let imported = items.iter().find(|item| item.label == "replace").unwrap();
        assert!(imported.import_edit.is_none());
        let first = "fn value() { let b = \"hi\"; b.tr }";
        let doc = Analysis::open(Some(&path), first);
        let item = doc
            .completions(first.find("b.tr").unwrap() + 4)
            .into_iter()
            .find(|item| item.label == "trim")
            .unwrap();
        let (span, text) = item.import_edit.unwrap();
        assert_eq!(span.start, 0);
        assert_eq!(text, "import { trim } from \"std/string\";\n\n");
    }

    #[test]
    fn a_let_stays_inside_its_block() {
        let source = "fn main() -> i16 {\n  let x = 1;\n  {\n    let y = x;\n    y\n  }\n  x\n}\n";
        let doc = Analysis::open(None, source);
        assert!(
            doc.diagnostics.is_empty(),
            "{:?}",
            doc.diagnostics
                .iter()
                .map(|err| err.to_string())
                .collect::<Vec<_>>()
        );
        let inside = source.find("y\n").unwrap();
        let names: Vec<_> = doc
            .visible(inside)
            .iter()
            .map(|fact| fact.name.as_str())
            .collect();
        assert!(names.contains(&"x"), "{names:?}");
        assert!(names.contains(&"y"), "{names:?}");
        let after = source.rfind('x').unwrap();
        let names: Vec<_> = doc
            .visible(after)
            .iter()
            .map(|fact| fact.name.as_str())
            .collect();
        assert!(names.contains(&"x"), "{names:?}");
        assert!(!names.contains(&"y"), "{names:?}");
        let hover = doc.hover(after).unwrap();
        assert!(hover.contains("i16"), "{hover}");
    }

    #[test]
    fn completion_after_a_dot_lists_methods() {
        let source = "fn main() {\n  let mut tape: Vec<u16> = [];\n  tape.l\n  let n = 1;\n}\n";
        let doc = Analysis::open(None, source);
        let typed = source.find("tape.l").unwrap() + "tape.l".len();
        let labels: Vec<String> = doc
            .completions(typed)
            .into_iter()
            .map(|item| item.label)
            .collect();
        assert!(
            labels.iter().any(|label| label == "len"),
            "{labels:?}\n{}",
            doc.diagnostics
                .iter()
                .map(|err| err.to_string())
                .collect::<Vec<_>>()
                .join("\n")
        );
        assert!(labels.iter().any(|label| label == "push"), "{labels:?}");
        assert!(!labels.iter().any(|label| label == "let"), "{labels:?}");
        let bare = source.find("tape.").unwrap() + "tape.".len();
        let labels: Vec<String> = doc
            .completions(bare)
            .into_iter()
            .map(|item| item.label)
            .collect();
        assert!(labels.iter().any(|label| label == "len"), "{labels:?}");
    }

    #[test]
    fn a_call_reports_the_checked_signature() {
        let source = "fn main() -> i16 {\n  let n = id(1);\n  n\n}\nfn id(x: i16) -> i16 { x }\n";
        let doc = Analysis::open(None, source);
        assert!(
            doc.diagnostics.is_empty(),
            "{:?}",
            doc.diagnostics
                .iter()
                .map(|err| err.to_string())
                .collect::<Vec<_>>()
        );
        let inside = source.find("id(1)").unwrap() + 3;
        let (label, active) = doc.signature(inside).unwrap();
        assert_eq!(label, "fn(i16) -> i16");
        assert_eq!(active, 0);
        let on_name = source.find("id(1)").unwrap();
        let hover = doc.hover(on_name).unwrap();
        assert!(hover.contains("fn(i16) -> i16"), "{hover}");
        let (_, def) = doc.definition(on_name).unwrap();
        let def_at = source[def.start..def.end].to_string();
        assert_eq!(def_at, "id");
        assert!(
            def.start > on_name,
            "definition should be the later function, at {}",
            def.start
        );
    }
}

#[cfg(test)]
mod resources {
    use super::Analysis;
    #[test]
    fn owners_supply_checked_types_methods_and_move_errors() {
        let source = "fn main() { let mut owner=owned(1,fn(x:&mut i16)=>{}); owner. }";
        let doc = Analysis::open(None, source);
        let items = doc.completions(source.find("owner.").unwrap() + 6);
        for (method, signature) in [
            ("borrow", "fn() -> &i16"),
            ("borrow_mut", "fn() -> &mut i16"),
            ("into_inner", "fn() -> i16"),
        ] {
            assert!(items
                .iter()
                .any(|i| i.label == method && i.detail == signature));
        }
        let source="fn main() { let owner=owned(1,fn(x:&mut i16)=>{}); let next=move(owner); owner.borrow(); }";
        let doc = Analysis::open(None, source);
        assert!(doc
            .diagnostics
            .iter()
            .any(|e| e.detail.contains("moved value")));
        let moved = source.find("move(owner)").unwrap() + 5;
        let (_, span) = doc.definition(moved).unwrap();
        assert_eq!(&source[span.start..span.end], "owner");
    }
}
