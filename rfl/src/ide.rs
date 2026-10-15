//! Queries for the language server. Positions are byte offsets into `source`.
//! Scope and type information comes from the parser's spans and the checker.

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
}

impl Analysis {
    pub fn open(path: Option<&Path>, source: &str) -> Self {
        let parsed = match lexer::tokenize(source).and_then(|tokens| parser::parse(source, &tokens)) {
            Ok(ast) => ast,
            Err(err) => {
                return Self {
                    source: source.to_string(),
                    path: path.map(|path| path.to_path_buf()),
                    diagnostics: vec![err],
                    info: empty_info(),
                    symbols: Vec::new(),
                    ast: empty_module(),
                };
            }
        };
        let symbols = symbols_of(&parsed);
        let (info, diagnostics) = if parsed.imports.is_empty() || path.is_none() {
            let (info, result) = check::analyze(source, &parsed);
            (info, result.err().into_iter().collect())
        } else {
            match crate::modules::check_root(path.unwrap(), source) {
                Ok(checked) => checked,
                Err(err) => {
                    let (info, result) = check::analyze(source, &parsed);
                    let mut diagnostics = vec![err];
                    diagnostics.extend(result.err());
                    (info, diagnostics)
                }
            }
        };
        Self {
            source: source.to_string(),
            path: path.map(|path| path.to_path_buf()),
            diagnostics,
            info,
            symbols,
            ast: parsed,
        }
    }

    pub fn hover(&self, offset: usize) -> Option<String> {
        if let Some(name) = self.uses().find(|name| name.span.contains(offset)) {
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
            let path = name.def_file.clone().or_else(|| self.path.clone());
            return Some((path, name.def));
        }
        if let Some(fact) = self.facts().find(|fact| fact.def.contains(offset)) {
            return Some((self.path.clone(), fact.def));
        }
        self.field_definition(offset)
    }

    pub fn references(&self, offset: usize) -> Vec<SourceLoc> {
        let (def_file, def) = if let Some(name) = self.uses().find(|name| name.span.contains(offset)) {
            (name.def_file.clone(), name.def)
        } else if let Some(fact) = self.facts().find(|fact| fact.def.contains(offset)) {
            (None, fact.def)
        } else if let Some((span, _, _)) = self.field_at(offset) {
            return vec![SourceLoc { path: self.path.clone(), span }];
        } else {
            return Vec::new();
        };
        let mut locs = vec![SourceLoc { path: def_file.clone().or_else(|| self.path.clone()), span: def }];
        locs.extend(self.uses().filter(|name| name.def == def && name.def_file == def_file).map(|name| SourceLoc { path: self.path.clone(), span: name.span }));
        locs
    }

    pub fn completions(&self, offset: usize) -> Vec<Completion> {
        if let Some(dot) = self.dot_before(offset) {
            return self.method_completions(dot);
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
            } else {
                CompletionKind::Variable
            };
            items.push(Completion { label: fact.name.clone(), kind, detail: fact.ty.clone() });
        }
        for fact in self.facts().filter(|fact| fact.scope.contains(offset) && offset >= fact.visible_from && (fact.ty.starts_with("fn(") || fact.variant)) {
            if seen.contains(&fact.name) {
                continue;
            }
            seen.push(fact.name.clone());
            let kind = if fact.variant { CompletionKind::EnumMember } else { CompletionKind::Function };
            items.push(Completion { label: fact.name.clone(), kind, detail: fact.ty.clone() });
        }
        for symbol in &self.symbols {
            if seen.contains(&symbol.name) {
                continue;
            }
            seen.push(symbol.name.clone());
            items.push(Completion {
                label: symbol.name.clone(),
                kind: match symbol.kind {
                    SymbolKind::Function | SymbolKind::Method => CompletionKind::Function,
                    SymbolKind::Struct | SymbolKind::Enum => CompletionKind::Type,
                    SymbolKind::Const => CompletionKind::Constant,
                    SymbolKind::Field => CompletionKind::Field,
                },
                detail: symbol.detail.clone(),
            });
        }
        for word in KEYWORDS {
            if !seen.iter().any(|name| name == word) {
                items.push(Completion { label: (*word).to_string(), kind: CompletionKind::Keyword, detail: String::new() });
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
                self.facts().find(|fact| fact.name == name && fact.ty.starts_with("fn(")).map(|fact| fact.ty.clone())
            })?;
        let active = call.args.iter().position(|span| offset < span.end).unwrap_or(call.args.len()) as u32;
        Some((label, active))
    }

    pub fn token_type(&self, span: crate::ast::Span) -> u32 {
        if let Some(fact) = self.facts().find(|fact| fact.def == span) {
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
        if self.symbols.iter().any(|symbol| matches!(symbol.kind, SymbolKind::Struct | SymbolKind::Enum) && symbol.span == span) {
            return 2;
        }
        if let Some(name) = self.uses().find(|name| name.span == span) {
            if name.ty.starts_with("type ") {
                return 2;
            }
            if self.facts().any(|fact| fact.def == name.def && fact.variant) {
                return 8;
            }
            if name.ty.starts_with("fn(") {
                return 1;
            }
            if self.facts().any(|fact| fact.def == name.def && fact.def.end <= fact.scope.start) {
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
    }

    fn method_completions(&self, receiver_end: usize) -> Vec<Completion> {
        let Some(expr) = self.info.expr_types.iter().find(|expr| expr.span.end == receiver_end) else {
            return Vec::new();
        };
        let mut items = Vec::new();
        let ty = expr.ty.as_str();
        let builtin: &[&str] = if ty.starts_with("Vec<") || ty == "vec" {
            &["len", "push", "pop", "get", "is_empty", "clear"]
        } else if ty == "String" {
            &["len", "is_empty"]
        } else if ty.starts_with("Slice<") {
            &["len", "is_empty", "get"]
        } else {
            &[]
        };
        for name in builtin {
            items.push(Completion { label: (*name).to_string(), kind: CompletionKind::Method, detail: expr.ty.clone() });
        }
        for (type_name, method) in &self.info.methods {
            if ty == type_name || ty.starts_with(&format!("{type_name}<")) {
                items.push(Completion { label: method.clone(), kind: CompletionKind::Method, detail: type_name.clone() });
            }
        }
        items
    }

    fn dot_before(&self, offset: usize) -> Option<usize> {
        let mut index = offset;
        let bytes = self.source.as_bytes();
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
            .filter(|fact| fact.scope.contains(offset) && offset >= fact.visible_from && !fact.ty.starts_with("fn("))
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
                if let Some(symbol) = self.symbols.iter().find(|symbol| symbol.kind == SymbolKind::Field && symbol.name == found.1 && symbol.detail == found.2) {
                    return Some((symbol.span, found.1, found.2));
                }
            }
        }
        None
    }

    fn expr_at(&self, offset: usize) -> Option<&ExprType> {
        self.info.expr_types.iter().filter(|expr| expr.span.contains(offset)).min_by_key(|expr| expr.span.end - expr.span.start)
    }

    fn facts(&self) -> impl Iterator<Item = &BindingFact> {
        self.info.facts.iter()
    }

    fn uses(&self) -> impl Iterator<Item = &NameUse> {
        self.info.uses.iter()
    }

    fn function_bodies(&self) -> Vec<&Block> {
        self.ast.functions.iter().map(|func| &func.body).collect()
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

fn field_in_block(block: &Block, offset: usize, types: &[ExprType]) -> Option<(crate::ast::Span, String, String)> {
    for stmt in &block.stmts {
        if let Some(found) = field_in_stmt(stmt, offset, types) {
            return Some(found);
        }
    }
    block.tail.as_ref().and_then(|expr| field_in_expr(expr, offset, types))
}

fn field_in_stmt(stmt: &Stmt, offset: usize, types: &[ExprType]) -> Option<(crate::ast::Span, String, String)> {
    match stmt {
        Stmt::Let { init, .. } => field_in_expr(init, offset, types),
        Stmt::Assign { value, .. } | Stmt::DerefAssign { value, .. } | Stmt::FieldAssign { value, .. } => field_in_expr(value, offset, types),
        Stmt::IndexAssign { index, value, .. } => field_in_expr(index, offset, types).or_else(|| field_in_expr(value, offset, types)),
        Stmt::Expr(expr) | Stmt::Return { value: Some(expr), .. } => field_in_expr(expr, offset, types),
        _ => None,
    }
}

fn field_in_expr(expr: &Expr, offset: usize, types: &[ExprType]) -> Option<(crate::ast::Span, String, String)> {
    if !expr.span.contains(offset) {
        return None;
    }
    match &expr.kind {
        ExprKind::Field { base, name, name_span } if name_span.contains(offset) => {
            let ty = types.iter().find(|expr| expr.span == base.span).map(|expr| expr.ty.clone()).unwrap_or_default();
            Some((*name_span, name.clone(), ty))
        }
        ExprKind::Field { base, .. } => field_in_expr(base, offset, types),
        ExprKind::Call { args, .. } | ExprKind::Array(args) | ExprKind::Tuple(args) | ExprKind::Assoc { args, .. } => {
            args.iter().find_map(|arg| field_in_expr(arg, offset, types))
        }
        ExprKind::StructLit { fields, .. } => fields.iter().find_map(|(_, expr)| field_in_expr(expr, offset, types)),
        ExprKind::Lambda { body, .. } => field_in_expr(body, offset, types),
        ExprKind::Method { receiver, args, .. } => field_in_expr(receiver, offset, types).or_else(|| args.iter().find_map(|arg| field_in_expr(arg, offset, types))),
        ExprKind::Binary { lhs, rhs, .. } => field_in_expr(lhs, offset, types).or_else(|| field_in_expr(rhs, offset, types)),
        ExprKind::Unary { expr, .. } | ExprKind::Try(expr) | ExprKind::Deref(expr) | ExprKind::Borrow { expr, .. } => field_in_expr(expr, offset, types),
        ExprKind::Index { base, index } => field_in_expr(base, offset, types).or_else(|| field_in_expr(index, offset, types)),
        ExprKind::If { cond, then_body, else_body } => field_in_expr(cond, offset, types)
            .or_else(|| field_in_block(then_body, offset, types))
            .or_else(|| else_body.as_ref().and_then(|expr| field_in_expr(expr, offset, types))),
        ExprKind::Block(block) | ExprKind::Unsafe(block) => field_in_block(block, offset, types),
        ExprKind::While { cond, body } => field_in_expr(cond, offset, types).or_else(|| field_in_block(body, offset, types)),
        ExprKind::For { iter, body, .. } => field_in_expr(iter, offset, types).or_else(|| field_in_block(body, offset, types)),
        ExprKind::Match { scrutinee, arms } => field_in_expr(scrutinee, offset, types).or_else(|| {
            arms.iter().find_map(|arm| arm.guard.as_ref().and_then(|guard| field_in_expr(guard, offset, types)).or_else(|| field_in_expr(&arm.body, offset, types)))
        }),
        _ => None,
    }
}

fn call_in_stmt(stmt: &Stmt, offset: usize) -> Option<CallAt> {
    match stmt {
        Stmt::Let { init, .. } => call_in_expr(init, offset),
        Stmt::Assign { value, .. } | Stmt::DerefAssign { value, .. } | Stmt::FieldAssign { value, .. } => call_in_expr(value, offset),
        Stmt::IndexAssign { index, value, .. } => call_in_expr(index, offset).or_else(|| call_in_expr(value, offset)),
        Stmt::Expr(expr) | Stmt::Return { value: Some(expr), .. } => call_in_expr(expr, offset),
        Stmt::Return { value: None, .. } | Stmt::Break { .. } | Stmt::Continue { .. } => None,
    }
}

fn call_in_expr(expr: &Expr, offset: usize) -> Option<CallAt> {
    if !expr.span.contains(offset) {
        return None;
    }
    match &expr.kind {
        ExprKind::Call { callee_span, args, .. } if callee_span.end <= offset => {
            for arg in args {
                if let Some(found) = call_in_expr(arg, offset) {
                    return Some(found);
                }
            }
            Some(CallAt { span: expr.span, callee: *callee_span, args: args.iter().map(|arg| arg.span).collect() })
        }
        ExprKind::Method { receiver, method_span, args, .. } => {
            if let Some(found) = call_in_expr(receiver, offset) {
                return Some(found);
            }
            for arg in args {
                if let Some(found) = call_in_expr(arg, offset) {
                    return Some(found);
                }
            }
            if offset >= method_span.start {
                Some(CallAt { span: expr.span, callee: *method_span, args: args.iter().map(|arg| arg.span).collect() })
            } else {
                None
            }
        }
        ExprKind::Binary { lhs, rhs, .. } => call_in_expr(lhs, offset).or_else(|| call_in_expr(rhs, offset)),
        ExprKind::Unary { expr, .. } | ExprKind::Try(expr) | ExprKind::Borrow { expr, .. } | ExprKind::Deref(expr) => call_in_expr(expr, offset),
        ExprKind::If { cond, then_body, else_body } => {
            call_in_expr(cond, offset).or_else(|| call_in_block(then_body, offset)).or_else(|| else_body.as_ref().and_then(|expr| call_in_expr(expr, offset)))
        }
        ExprKind::Block(block) | ExprKind::Unsafe(block) => call_in_block(block, offset),
        ExprKind::While { cond, body } => call_in_expr(cond, offset).or_else(|| call_in_block(body, offset)),
        ExprKind::For { iter, body, .. } => call_in_expr(iter, offset).or_else(|| call_in_block(body, offset)),
        ExprKind::Match { scrutinee, arms } => call_in_expr(scrutinee, offset).or_else(|| {
            arms.iter().find_map(|arm| arm.guard.as_ref().and_then(|guard| call_in_expr(guard, offset)).or_else(|| call_in_expr(&arm.body, offset)))
        }),
        ExprKind::Index { base, index } => call_in_expr(base, offset).or_else(|| call_in_expr(index, offset)),
        ExprKind::Field { base, .. } => call_in_expr(base, offset),
        ExprKind::Array(elems) | ExprKind::Tuple(elems) => elems.iter().find_map(|elem| call_in_expr(elem, offset)),
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
    block.tail.as_ref().and_then(|expr| call_in_expr(expr, offset))
}

fn hints_in_block(block: &Block, facts: &[BindingFact], hints: &mut Vec<(usize, String)>) {
    for stmt in &block.stmts {
        if let Stmt::Let { pattern, ty: None, .. } = stmt {
            if let crate::ast::Pat::Name { name, span: name_span } = pattern {
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
        Stmt::Assign { value, .. } | Stmt::DerefAssign { value, .. } | Stmt::FieldAssign { value, .. } => hints_in_expr(value, facts, hints),
        Stmt::IndexAssign { index, value, .. } => {
            hints_in_expr(index, facts, hints);
            hints_in_expr(value, facts, hints);
        }
        Stmt::Expr(expr) | Stmt::Return { value: Some(expr), .. } => hints_in_expr(expr, facts, hints),
        _ => {}
    }
}

fn hints_in_expr(expr: &Expr, facts: &[BindingFact], hints: &mut Vec<(usize, String)>) {
    match &expr.kind {
        ExprKind::Block(block) | ExprKind::Unsafe(block) => hints_in_block(block, facts, hints),
        ExprKind::If { then_body, else_body, cond } => {
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
        ExprKind::Unary { expr, .. } | ExprKind::Try(expr) | ExprKind::Deref(expr) | ExprKind::Borrow { expr, .. } | ExprKind::Field { base: expr, .. } => {
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
}

#[derive(Clone, Copy)]
pub enum CompletionKind {
    Keyword,
    Function,
    Method,
    Variable,
    Type,
    Constant,
    Field,
    EnumMember,
}

const KEYWORDS: &[&str] = &[
    "fn", "let", "mut", "if", "else", "return", "as", "while", "break", "continue", "match", "for", "in", "type", "impl", "const", "struct",
    "export", "import", "extern", "unsafe", "true", "false",
];

fn symbols_of(module: &Module) -> Vec<Symbol> {
    let mut symbols = Vec::new();
    for func in &module.functions {
        push_function(&mut symbols, func, SymbolKind::Function);
    }
    for item in &module.impls {
        for method in &item.items {
            symbols.push(Symbol {
                name: method.name.clone(),
                kind: SymbolKind::Method,
                span: crate::ast::Span::new(method.name_span.start, method.body.span.end),
                detail: String::new(),
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
        span: crate::ast::Span::new(func.name_span.start, func.body.span.end.max(func.name_span.end)),
        detail: String::new(),
    });
}

fn empty_info() -> ProgramInfo {
    ProgramInfo { facts: Vec::new(), uses: Vec::new(), expr_types: Vec::new(), methods: Vec::new(), call_sigs: Vec::new() }
}

fn empty_module() -> Module {
    Module { adts: Vec::new(), structs: Vec::new(), impls: Vec::new(), consts: Vec::new(), functions: Vec::new(), externs: Vec::new(), imports: Vec::new() }
}

#[cfg(test)]
mod tests {
    use super::Analysis;

    #[test]
    fn a_let_stays_inside_its_block() {
        let source = "fn main() -> i16 {\n  let x = 1;\n  {\n    let y = x;\n    y\n  }\n  x\n}\n";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics.iter().map(|err| err.to_string()).collect::<Vec<_>>());
        let inside = source.find("y\n").unwrap();
        let names: Vec<_> = doc.visible(inside).iter().map(|fact| fact.name.as_str()).collect();
        assert!(names.contains(&"x"), "{names:?}");
        assert!(names.contains(&"y"), "{names:?}");
        let after = source.rfind('x').unwrap();
        let names: Vec<_> = doc.visible(after).iter().map(|fact| fact.name.as_str()).collect();
        assert!(names.contains(&"x"), "{names:?}");
        assert!(!names.contains(&"y"), "{names:?}");
        let hover = doc.hover(after).unwrap();
        assert!(hover.contains("i16"), "{hover}");
    }

    #[test]
    fn a_call_reports_the_checked_signature() {
        let source = "fn main() -> i16 {\n  let n = id(1);\n  n\n}\nfn id(x: i16) -> i16 { x }\n";
        let doc = Analysis::open(None, source);
        assert!(doc.diagnostics.is_empty(), "{:?}", doc.diagnostics.iter().map(|err| err.to_string()).collect::<Vec<_>>());
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
        assert!(def.start > on_name, "definition should be the later function, at {}", def.start);
    }
}
