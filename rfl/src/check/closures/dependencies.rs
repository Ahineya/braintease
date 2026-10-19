//! Find lexical dependencies before lifting a nested closure. Every intervening
//! environment must carry a free binding even if only the innermost body uses it.
use super::*;

pub(super) fn free_names(params: &[crate::ast::Param], body: &Expr) -> Vec<(String, usize)> {
    let mut walk = Dependencies {
        scopes: vec![HashSet::new()],
        names: Vec::new(),
        seen: HashSet::new(),
    };
    for param in params {
        walk.bind(&param.pat);
    }
    walk.expr(body);
    walk.names
}
struct Dependencies {
    scopes: Vec<HashSet<String>>,
    names: Vec<(String, usize)>,
    seen: HashSet<String>,
}
impl Dependencies {
    fn name(&mut self, name: &str, span: usize) {
        if !self.scopes.iter().rev().any(|s| s.contains(name)) && self.seen.insert(name.to_owned())
        {
            self.names.push((name.to_owned(), span));
        }
    }
    fn bind(&mut self, pat: &Pat) {
        let mut names = Vec::new();
        pat_names(pat, &mut names);
        self.scopes
            .last_mut()
            .unwrap()
            .extend(names.into_iter().map(|(n, _)| n));
    }
    fn block(&mut self, block: &Block) {
        self.scopes.push(HashSet::new());
        for stmt in &block.stmts {
            match stmt {
                Stmt::Let { pattern, init, .. } => {
                    self.expr(init);
                    self.bind(pattern);
                }
                Stmt::Assign {
                    name,
                    name_span,
                    value,
                } => {
                    self.name(name, name_span.start);
                    self.expr(value);
                }
                Stmt::IndexAssign {
                    name, index, value, ..
                } => {
                    self.name(name, index.span.start);
                    self.expr(index);
                    self.expr(value);
                }
                Stmt::FieldAssign { name, value, .. } => {
                    self.name(name, value.span.start);
                    self.expr(value);
                }
                Stmt::DerefAssign { ptr, value } => {
                    self.expr(ptr);
                    self.expr(value);
                }
                Stmt::Expr(expr)
                | Stmt::Return {
                    value: Some(expr), ..
                } => self.expr(expr),
                Stmt::Return { value: None, .. } | Stmt::Break { .. } | Stmt::Continue { .. } => {}
            }
        }
        if let Some(tail) = &block.tail {
            self.expr(tail);
        }
        self.scopes.pop();
    }
    fn expr(&mut self, expr: &Expr) {
        match &expr.kind {
            ExprKind::Name(name) => self.name(name, expr.span.start),
            ExprKind::Call {
                callee,
                callee_span,
                args,
                ..
            } => {
                self.name(callee, callee_span.start);
                for arg in args {
                    self.expr(arg);
                }
            }
            ExprKind::Assoc { args, .. } | ExprKind::Tuple(args) | ExprKind::Array(args) => {
                for arg in args {
                    self.expr(arg);
                }
            }
            ExprKind::Method { receiver, args, .. } => {
                self.expr(receiver);
                for arg in args {
                    self.expr(arg);
                }
            }
            ExprKind::Unary { expr, .. }
            | ExprKind::Borrow { expr, .. }
            | ExprKind::Cast { expr, .. }
            | ExprKind::Deref(expr)
            | ExprKind::Try(expr)
            | ExprKind::Await(expr) => self.expr(expr),
            ExprKind::Binary { lhs, rhs, .. }
            | ExprKind::Range {
                start: lhs,
                end: rhs,
                ..
            } => {
                self.expr(lhs);
                self.expr(rhs);
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.block(block),
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                self.expr(cond);
                self.block(then_body);
                if let Some(body) = else_body {
                    self.expr(body);
                }
            }
            ExprKind::Match { scrutinee, arms } => {
                self.expr(scrutinee);
                for arm in arms {
                    self.scopes.push(HashSet::new());
                    self.bind(&arm.pat);
                    if let Some(guard) = &arm.guard {
                        self.expr(guard);
                    }
                    self.expr(&arm.body);
                    self.scopes.pop();
                }
            }
            ExprKind::While { cond, body } => {
                self.expr(cond);
                self.block(body);
            }
            ExprKind::For { pat, iter, body } => {
                self.expr(iter);
                self.scopes.push(HashSet::new());
                self.bind(pat);
                self.block(body);
                self.scopes.pop();
            }
            ExprKind::Lambda { params, body, .. } => {
                self.scopes.push(HashSet::new());
                for param in params {
                    self.bind(&param.pat);
                }
                self.expr(body);
                self.scopes.pop();
            }
            ExprKind::StructLit { fields, .. } => {
                for (_, value) in fields {
                    self.expr(value);
                }
            }
            ExprKind::Field { base, .. } => self.expr(base),
            ExprKind::Index { base, index } => {
                self.expr(base);
                self.expr(index);
            }
            ExprKind::Slice {
                base, start, end, ..
            } => {
                self.expr(base);
                self.expr(start);
                self.expr(end);
            }
            ExprKind::Macro(_) => {}
            ExprKind::Int(_)
            | ExprKind::Float(_)
            | ExprKind::Bool(_)
            | ExprKind::Char(_)
            | ExprKind::Str(_)
            | ExprKind::CStr(_) => {}
        }
    }
}
