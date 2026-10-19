//! Parser-derived layout hints. No name resolution or import loading is needed.
use std::collections::{HashMap, HashSet};

use crate::ast::*;
use crate::error::CompileError;
use crate::lexer::{Token, TokenKind as K};

#[derive(Default)]
pub(super) struct Syntax {
    pub pairs: HashMap<usize, usize>,
    pub tight: HashSet<usize>,
    pub generic_commas: HashSet<usize>,
    pub inline_braces: HashSet<usize>,
    pub lines: HashSet<usize>,
    pub joined: HashSet<usize>,
    pub html_bodies: HashSet<usize>,
    pub html_names: HashSet<String>,
    opaque_macros: HashSet<String>,
}

impl Syntax {
    pub fn analyze(source: &str, tokens: &[Token]) -> Result<Self, CompileError> {
        let code: Vec<_> = tokens
            .iter()
            .filter(|t| !matches!(t.kind, K::LineComment | K::BlockComment))
            .cloned()
            .collect();
        if code.len() == 1 && code[0].kind == K::Eof {
            return Ok(Self::default());
        }
        let module = crate::parser::parse(source, &code)?;
        let mut result = Self::default();
        let mut stack = Vec::new();
        for (i, token) in tokens.iter().enumerate() {
            match token.kind {
                K::LParen | K::LBracket | K::LBrace => stack.push(i),
                K::RParen | K::RBracket | K::RBrace => {
                    let open = stack.pop().ok_or_else(|| {
                        CompileError::at(source, token.span.start, "unmatched delimiter")
                    })?;
                    result.pairs.insert(open, i);
                }
                _ => {}
            }
        }
        for pair in tokens.windows(2) {
            if matches!(pair[0].kind, K::Ident(_)) && pair[1].kind == K::Not {
                result.joined.insert(pair[1].span.start);
            }
        }
        result.module(&module, tokens);
        // Include declarations/imports discarded by the default target. Layout
        // is source-only, including browser-only imports and their aliases.
        for target in [rcc_ir::target::Target::Native, rcc_ir::target::Target::Wasm] {
            let module = crate::parser::parse_for_target(source, &code, target)?;
            result.module(&module, tokens);
        }
        result
            .html_names
            .retain(|name| !result.opaque_macros.contains(name));
        let code: Vec<_> = tokens
            .iter()
            .filter(|t| !matches!(t.kind, K::LineComment | K::BlockComment))
            .collect();
        for call in code.windows(3) {
            if let K::Ident(name) = &call[0].kind {
                if result.html_names.contains(name)
                    && call[1].kind == K::Not
                    && matches!(call[2].kind, K::MacroBody(_))
                {
                    result.html_bodies.insert(call[2].span.start);
                }
            }
        }
        Ok(result)
    }

    fn first(&self, tokens: &[Token], at: usize, kind: K) -> Option<usize> {
        let i = tokens.partition_point(|t| t.span.start < at);
        tokens[i..]
            .iter()
            .position(|t| t.kind == kind)
            .map(|n| n + i)
    }

    fn inline(&mut self, tokens: &[Token], at: usize) {
        if let Some(i) = self.first(tokens, at, K::LBrace) {
            self.inline_braces.insert(tokens[i].span.start);
        }
    }

    fn generic(&mut self, tokens: &[Token], at: usize) {
        let i = tokens.partition_point(|t| t.span.start < at);
        let i = (i..tokens.len())
            .find(|i| !matches!(tokens[*i].kind, K::LineComment | K::BlockComment));
        let Some(i) = i else { return };
        if !matches!(tokens[i].kind, K::Lt | K::Shl) {
            return;
        }
        let mut depth = 0i32;
        let mut nested = 0usize;
        for token in &tokens[i..] {
            // Array lengths are expressions and may contain comparisons or
            // shifts. Their operators do not delimit the enclosing generic.
            match token.kind {
                K::LParen | K::LBracket | K::LBrace => {
                    nested += 1;
                    continue;
                }
                K::RParen | K::RBracket | K::RBrace => {
                    nested = nested.saturating_sub(1);
                    continue;
                }
                _ if nested > 0 => continue,
                _ => {}
            }
            match token.kind {
                K::Lt => depth += 1,
                K::Shl => depth += 2,
                K::Gt => depth -= 1,
                K::Shr => depth -= 2,
                K::Comma => {
                    // Generic lists do not have delimiter frames in the printer.
                    self.generic_commas.insert(token.span.start);
                    continue;
                }
                _ => continue,
            }
            self.tight.insert(token.span.start);
            if depth <= 0 {
                break;
            }
        }
    }

    fn ty(&mut self, ty: &ParsedTy, tokens: &[Token]) {
        match ty {
            ParsedTy::Named { span, args, .. } => {
                if !args.is_empty() {
                    self.generic(tokens, span.end);
                }
                for arg in args {
                    self.ty(arg, tokens);
                }
            }
            ParsedTy::Projection { span, base, .. } => {
                self.generic(tokens, span.start);
                self.ty(base, tokens);
            }
            ParsedTy::Ref { span, inner, .. } | ParsedTy::Ptr { span, inner, .. } => {
                self.tight.insert(span.start);
                self.ty(inner, tokens);
            }
            ParsedTy::Array { elem, len, .. } => {
                self.ty(elem, tokens);
                self.expr(len, tokens);
            }
            ParsedTy::Tuple { fields, .. } => {
                for field in fields {
                    self.ty(field, tokens);
                }
            }
            ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
                for param in params {
                    self.ty(param, tokens);
                }
                self.ty(ret, tokens);
            }
        }
    }

    fn params(&mut self, params: &[Param], tokens: &[Token]) {
        for param in params {
            if let Some(ty) = &param.ty {
                self.ty(ty, tokens);
            }
            self.pat(&param.pat, tokens);
        }
    }

    fn method(&mut self, method: &ImplItem, tokens: &[Token]) {
        self.generic(tokens, method.name_span.end);
        self.params(&method.params, tokens);
        if let Some(ty) = &method.ret {
            self.ty(ty, tokens);
        }
        self.block(&method.body, tokens);
        // Receivers aren't represented by Param/ParsedTy nodes.
        if let Some(open) = self.first(tokens, method.name_span.end, K::LParen) {
            if let Some(amp) = tokens.get(open + 1).filter(|t| t.kind == K::Amp) {
                self.tight.insert(amp.span.start);
            }
        }
    }

    fn module(&mut self, module: &Module, tokens: &[Token]) {
        for import in &module.macro_imports {
            for name in &import.names {
                if import.path.trim_end_matches(".rfl") == "std/html_macro" && name.name == "html" {
                    self.html_names.insert(name.alias.clone());
                } else {
                    // One spelling may select a different DSL under another
                    // target cfg. Never impose standard HTML whitespace there.
                    self.opaque_macros.insert(name.alias.clone());
                }
            }
        }
        for import in module.imports.iter().chain(&module.macro_imports) {
            if !import.is_implicit() {
                self.inline(tokens, import.span.start);
            }
        }
        for item in &module.adts {
            self.generic(tokens, item.name_span.end);
            for ctor in &item.ctors {
                for ty in &ctor.fields {
                    self.ty(ty, tokens);
                }
            }
        }
        for item in &module.structs {
            for field in &item.fields {
                self.ty(&field.ty, tokens);
            }
        }
        for item in &module.consts {
            self.ty(&item.ty, tokens);
            self.expr(&item.init, tokens);
        }
        for item in &module.functions {
            self.generic(tokens, item.name_span.end);
            self.params(&item.params, tokens);
            if let Some(ty) = &item.ret {
                self.ty(ty, tokens);
            }
            self.block(&item.body, tokens);
        }
        for item in &module.externs {
            self.params(&item.params, tokens);
            if let Some(ty) = &item.ret {
                self.ty(ty, tokens);
            }
        }
        for item in &module.impls {
            if let Some(i) = self.first(tokens, item.span.start, K::Impl) {
                self.generic(tokens, tokens[i].span.end);
            }
            self.ty(&item.ty, tokens);
            for assoc in &item.associated_types {
                if let Some(ty) = &assoc.value {
                    self.ty(ty, tokens);
                }
            }
            for method in &item.items {
                self.method(method, tokens);
            }
        }
        for item in &module.traits {
            self.generic(tokens, item.name_span.end);
            for method in &item.methods {
                self.method(method, tokens);
            }
        }
    }

    fn pat(&mut self, pat: &Pat, tokens: &[Token]) {
        match pat {
            Pat::Struct { span, fields, .. } => {
                self.inline(tokens, span.start);
                for (_, _, field) in fields {
                    self.pat(field, tokens);
                }
            }
            Pat::Tuple { fields, .. } | Pat::Ctor { fields, .. } => {
                for field in fields {
                    self.pat(field, tokens);
                }
            }
            Pat::Int { span, value, .. } if *value < 0 => {
                self.tight.insert(span.start);
            }
            _ => {}
        }
    }

    fn block(&mut self, block: &Block, tokens: &[Token]) {
        for stmt in &block.stmts {
            match stmt {
                Stmt::Let {
                    pattern, ty, init, ..
                } => {
                    self.pat(pattern, tokens);
                    if let Some(ty) = ty {
                        self.ty(ty, tokens);
                    }
                    self.expr(init, tokens);
                }
                Stmt::Assign { value, .. } | Stmt::FieldAssign { value, .. } => {
                    self.expr(value, tokens)
                }
                Stmt::DerefAssign { ptr, value } => {
                    // The assignment node drops the leading dereference token.
                    if let Some(i) = tokens.iter().rposition(|t| t.span.end <= ptr.span.start) {
                        if tokens[i].kind == K::Star {
                            self.tight.insert(tokens[i].span.start);
                        }
                    }
                    self.expr(ptr, tokens);
                    self.expr(value, tokens);
                }
                Stmt::IndexAssign { index, value, .. } => {
                    self.expr(index, tokens);
                    self.expr(value, tokens);
                }
                Stmt::Expr(expr)
                | Stmt::Return {
                    value: Some(expr), ..
                } => self.expr(expr, tokens),
                _ => {}
            }
        }
        if let Some(expr) = &block.tail {
            self.expr(expr, tokens);
        }
    }

    fn expr(&mut self, expr: &Expr, tokens: &[Token]) {
        match &expr.kind {
            ExprKind::Unary { expr: inner, .. }
            | ExprKind::Borrow { expr: inner, .. }
            | ExprKind::Deref(inner) => {
                self.tight.insert(expr.span.start);
                self.expr(inner, tokens);
            }
            ExprKind::Await(inner) | ExprKind::Try(inner) => self.expr(inner, tokens),
            ExprKind::Binary { lhs, rhs, .. } => {
                self.expr(lhs, tokens);
                self.expr(rhs, tokens);
            }
            ExprKind::Call {
                callee_span,
                type_args,
                args,
                ..
            } => {
                if !type_args.is_empty() && callee_span.end > callee_span.start {
                    self.generic(tokens, callee_span.end);
                }
                for ty in type_args {
                    self.ty(ty, tokens);
                }
                for arg in args {
                    self.expr(arg, tokens);
                }
            }
            ExprKind::Method { receiver, args, .. } => {
                self.expr(receiver, tokens);
                for arg in args {
                    self.expr(arg, tokens);
                }
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                self.expr(cond, tokens);
                self.block(then_body, tokens);
                if let Some(other) = else_body {
                    self.expr(other, tokens);
                }
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => {
                let i = tokens.partition_point(|t| t.span.start < expr.span.start);
                if tokens.get(i).is_some_and(|t| t.kind == K::For) {
                    let next = if tokens
                        .get(i + 1)
                        .is_some_and(|t| matches!(&t.kind, K::Ident(n) if n == "once"))
                    {
                        i + 2
                    } else {
                        i + 1
                    };
                    if let Some(token) = tokens.get(next) {
                        self.generic(tokens, token.span.start);
                    }
                }
                self.block(block, tokens);
            }
            ExprKind::While { cond, body } => {
                self.expr(cond, tokens);
                self.block(body, tokens);
            }
            ExprKind::For { pat, iter, body } => {
                self.pat(pat, tokens);
                self.expr(iter, tokens);
                self.block(body, tokens);
            }
            ExprKind::Match { scrutinee, arms } => {
                self.expr(scrutinee, tokens);
                for arm in arms {
                    self.lines.insert(arm.pat.span().start);
                    self.pat(&arm.pat, tokens);
                    if let Some(guard) = &arm.guard {
                        self.expr(guard, tokens);
                    }
                    self.expr(&arm.body, tokens);
                }
            }
            ExprKind::Tuple(items) | ExprKind::Array(items) => {
                for item in items {
                    self.expr(item, tokens);
                }
            }
            ExprKind::StructLit { fields, .. } => {
                self.inline(tokens, expr.span.start);
                for (_, value) in fields {
                    self.expr(value, tokens);
                }
            }
            ExprKind::Field { base, .. } => self.expr(base, tokens),
            ExprKind::Range { start, end, .. } => {
                self.expr(start, tokens);
                self.expr(end, tokens);
            }
            ExprKind::Cast { expr, ty } => {
                self.expr(expr, tokens);
                self.ty(ty, tokens);
            }
            ExprKind::Lambda { params, body, .. } => {
                // Generated continuation spans retain their original `<-`.
                let i = tokens.partition_point(|t| t.span.start < expr.span.start);
                if tokens.get(i).is_some_and(|t| t.kind == K::Lt)
                    && tokens
                        .get(i + 1)
                        .is_some_and(|t| t.kind == K::Minus && tokens[i].span.end == t.span.start)
                {
                    self.joined.insert(tokens[i + 1].span.start);
                }
                self.params(params, tokens);
                self.expr(body, tokens);
            }
            ExprKind::Index { base, index } => {
                self.expr(base, tokens);
                self.expr(index, tokens);
            }
            ExprKind::Slice {
                base, start, end, ..
            } => {
                self.expr(base, tokens);
                self.expr(start, tokens);
                self.expr(end, tokens);
            }
            ExprKind::Assoc { args, .. } => {
                for arg in args {
                    self.expr(arg, tokens);
                }
            }
            _ => {}
        }
    }
}
