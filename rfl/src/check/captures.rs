//! Captures helpers for RFL checking.

use super::*;

/// Spans of mutable bindings assigned from a nested closure.
pub(super) fn shared_assign_spans(body: &Block) -> HashSet<usize> {
    let mut found = HashSet::new();
    let mut scopes = vec![HashMap::new()];
    walk_block(body, &mut scopes, 0, &mut found);
    found
}

pub(super) fn walk_block(
    block: &Block,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    scopes.push(HashMap::new());
    for stmt in &block.stmts {
        walk_stmt(stmt, scopes, depth, found);
    }
    if let Some(tail) = &block.tail {
        walk_expr(tail, scopes, depth, found);
    }
    scopes.pop();
}

pub(super) fn walk_stmt(
    stmt: &Stmt,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    match stmt {
        Stmt::Let {
            pattern,
            mutable,
            init,
            ..
        } => {
            walk_expr(init, scopes, depth, found);
            bind_pat(pattern, *mutable, depth, scopes);
        }
        Stmt::Assign {
            name,
            name_span: _,
            value,
        } => {
            note_assign(scopes, name, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::IndexAssign {
            name, index, value, ..
        } => {
            note_assign(scopes, name, depth, found);
            walk_expr(index, scopes, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::FieldAssign { name, value, .. } => {
            note_assign(scopes, name, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::DerefAssign { ptr, value } => {
            walk_expr(ptr, scopes, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::Expr(expr) => walk_expr(expr, scopes, depth, found),
        Stmt::Return {
            value: Some(expr), ..
        } => walk_expr(expr, scopes, depth, found),
        Stmt::Return { value: None, .. } | Stmt::Break { .. } | Stmt::Continue { .. } => {}
    }
}

pub(super) fn note_assign(
    scopes: &[HashMap<String, (usize, bool, u32)>],
    name: &str,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    let Some((span, mutable, bind_depth)) = scopes
        .iter()
        .rev()
        .find_map(|scope| scope.get(name).copied())
    else {
        return;
    };
    if mutable && bind_depth < depth {
        found.insert(span);
    }
}

pub(super) fn bind_pat(
    pat: &Pat,
    mutable: bool,
    depth: u32,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
) {
    match pat {
        Pat::Name { name, span } if name != "_" => {
            scopes
                .last_mut()
                .unwrap()
                .insert(name.clone(), (span.start, mutable, depth));
        }
        Pat::Ctor { fields, .. } => {
            for field in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Tuple { fields, .. } => {
            for field in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Struct { fields, .. } => {
            for (_, _, field) in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Name { .. } | Pat::Int { .. } | Pat::Bool { .. } | Pat::Char { .. } => {}
    }
}

pub(super) fn walk_expr(
    expr: &Expr,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    match &expr.kind {
        ExprKind::Int(_)
        | ExprKind::Float(_)
        | ExprKind::Bool(_)
        | ExprKind::Str(_)
        | ExprKind::CStr(_)
        | ExprKind::Char(_)
        | ExprKind::Name(_) => {}
        ExprKind::Unary { expr, .. }
        | ExprKind::Try(expr)
        | ExprKind::Deref(expr)
        | ExprKind::Borrow { expr, .. } => {
            walk_expr(expr, scopes, depth, found);
        }
        ExprKind::Binary { lhs, rhs, .. }
        | ExprKind::Range {
            start: lhs,
            end: rhs,
            ..
        } => {
            walk_expr(lhs, scopes, depth, found);
            walk_expr(rhs, scopes, depth, found);
        }
        ExprKind::Call { args, .. }
        | ExprKind::Assoc { args, .. }
        | ExprKind::Tuple(args)
        | ExprKind::Array(args) => {
            for arg in args {
                walk_expr(arg, scopes, depth, found);
            }
        }
        ExprKind::Method { receiver, args, .. } => {
            walk_expr(receiver, scopes, depth, found);
            for arg in args {
                walk_expr(arg, scopes, depth, found);
            }
        }
        ExprKind::If {
            cond,
            then_body,
            else_body,
        } => {
            walk_expr(cond, scopes, depth, found);
            walk_block(then_body, scopes, depth, found);
            if let Some(else_body) = else_body {
                walk_expr(else_body, scopes, depth, found);
            }
        }
        ExprKind::Block(block) | ExprKind::Unsafe(block) => walk_block(block, scopes, depth, found),
        ExprKind::Match { scrutinee, arms } => {
            walk_expr(scrutinee, scopes, depth, found);
            for arm in arms {
                scopes.push(HashMap::new());
                bind_pat(&arm.pat, false, depth, scopes);
                if let Some(guard) = &arm.guard {
                    walk_expr(guard, scopes, depth, found);
                }
                walk_expr(&arm.body, scopes, depth, found);
                scopes.pop();
            }
        }
        ExprKind::While { cond, body } => {
            walk_expr(cond, scopes, depth, found);
            walk_block(body, scopes, depth, found);
        }
        ExprKind::For { pat, iter, body } => {
            walk_expr(iter, scopes, depth, found);
            scopes.push(HashMap::new());
            bind_pat(pat, false, depth, scopes);
            walk_block(body, scopes, depth, found);
            scopes.pop();
        }
        ExprKind::StructLit { fields, .. } => {
            for (_, field) in fields {
                walk_expr(field, scopes, depth, found);
            }
        }
        ExprKind::Field { base, .. } => walk_expr(base, scopes, depth, found),
        ExprKind::Cast { expr, .. } => walk_expr(expr, scopes, depth, found),
        ExprKind::Lambda { params, body } => {
            scopes.push(HashMap::new());
            for param in params {
                bind_pat(&param.pat, false, depth + 1, scopes);
            }
            walk_expr(body, scopes, depth + 1, found);
            scopes.pop();
        }
        ExprKind::Index { base, index } => {
            walk_expr(base, scopes, depth, found);
            walk_expr(index, scopes, depth, found);
        }
        ExprKind::Slice {
            base, start, end, ..
        } => {
            walk_expr(base, scopes, depth, found);
            walk_expr(start, scopes, depth, found);
            walk_expr(end, scopes, depth, found);
        }
    }
}
