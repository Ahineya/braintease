//! Capability checking consumes the real checker's resolved typed call graph.
//! This runs before CPS/native lowering can add global dispatch tables.
use crate::{typed::*, CompileError};
use std::collections::{HashMap, HashSet};
fn forbidden(service: &str) -> CompileError {
    CompileError::plain(format!(
        "macro deterministic profile forbids service `{service}`"
    ))
}
pub fn validate(module: &TModule) -> Result<(), CompileError> {
    let functions: HashMap<_, _> = module
        .functions
        .iter()
        .map(|f| (f.name.as_str(), f))
        .collect();
    let start = module
        .functions
        .iter()
        .find(|f| f.name.ends_with("$macro_dispatch") || f.name == "macro_dispatch")
        .ok_or_else(|| CompileError::plain("macro executor lost its checked entry point"))?;
    let mut work = vec![start.name.clone()];
    let mut seen = HashSet::new();
    while let Some(name) = work.pop() {
        if seen.insert(name.clone()) {
            let f = functions.get(name.as_str()).ok_or_else(|| {
                CompileError::plain(format!("macro profile cannot resolve `{name}`"))
            })?;
            block(&f.body, &mut work)?;
        }
    }
    Ok(())
}
fn block(b: &TBlock, refs: &mut Vec<String>) -> Result<(), CompileError> {
    for s in &b.stmts {
        statement(s, refs)?;
    }
    if let Some(e) = &b.tail {
        expr(e, refs)?;
    }
    Ok(())
}
fn statement(s: &TStmt, refs: &mut Vec<String>) -> Result<(), CompileError> {
    match s {
        TStmt::Let { init, .. }
        | TStmt::Destructure { init, .. }
        | TStmt::Assign { value: init, .. }
        | TStmt::FieldAssign { value: init, .. }
        | TStmt::Expr(init) => expr(init, refs)?,
        TStmt::IndexAssign { index, value, .. } => {
            expr(index, refs)?;
            expr(value, refs)?;
        }
        TStmt::PtrStore {
            ptr, index, value, ..
        } => {
            expr(ptr, refs)?;
            if let Some(i) = index {
                expr(i, refs)?;
            }
            expr(value, refs)?;
        }
        TStmt::Return(Some(e)) => expr(e, refs)?,
        TStmt::Return(None) | TStmt::Break | TStmt::Continue => {}
    }
    Ok(())
}
fn values(xs: &[TExpr], refs: &mut Vec<String>) -> Result<(), CompileError> {
    for x in xs {
        expr(x, refs)?;
    }
    Ok(())
}
fn expr(e: &TExpr, refs: &mut Vec<String>) -> Result<(), CompileError> {
    use TExprKind::*;
    match &e.kind {
        Print { .. } | PrintLit { .. } | Putc(_) => return Err(forbidden("stdout")),
        Extern { callee, .. } | CFn { symbol: callee } => return Err(forbidden(callee)),
        Call { callee, args } => {
            refs.push(callee.clone());
            values(args, refs)?;
        }
        Closure { func } => refs.push(func.clone()),
        AsyncNew { body, args } => {
            refs.push(body.clone());
            values(args, refs)?;
        }
        Expect { value, message, .. } => {
            expr(value, refs)?;
            expr(message, refs)?;
        }
        Await { owner, poll, .. } => {
            expr(owner, refs)?;
            expr(poll, refs)?;
        }
        AsyncPoll { future, context } => {
            expr(future, refs)?;
            expr(context, refs)?;
        }
        AsyncCancel { future } => expr(future, refs)?,
        Owned { value, cleanup } => {
            expr(value, refs)?;
            expr(cleanup, refs)?;
        }
        Replace { place, value } => {
            expr(place, refs)?;
            expr(value, refs)?;
        }
        Unary { expr: x, .. }
        | Cast { expr: x, .. }
        | Try { expr: x, .. }
        | Drop(x)
        | IntoInner(x)
        | EraseOwner(x)
        | RawBorrow(x)
        | StrFromChar(x)
        | StrFromByte(x) => expr(x, refs)?,
        Binary { lhs, rhs, .. } | StrConcat { lhs, rhs } => {
            expr(lhs, refs)?;
            expr(rhs, refs)?;
        }
        Apply { callee, args, .. } | CApply { callee, args } => {
            expr(callee, refs)?;
            values(args, refs)?;
        }
        If {
            cond,
            then_body,
            else_body,
        } => {
            expr(cond, refs)?;
            block(then_body, refs)?;
            if let Some(x) = else_body {
                expr(x, refs)?;
            }
        }
        Block(b) => block(b, refs)?,
        While { cond, body } => {
            expr(cond, refs)?;
            block(body, refs)?;
        }
        ForIter {
            start,
            end,
            extract,
            body,
            ..
        } => {
            expr(start, refs)?;
            expr(end, refs)?;
            if let Some(s) = extract {
                statement(s, refs)?;
            }
            block(body, refs)?;
        }
        Record { fields } => values(fields, refs)?,
        Ctor { args, .. } => values(args, refs)?,
        VecLit { elems, .. } | ArrayLit { elems, .. } => values(elems, refs)?,
        Field { base, .. } | Len { vec: base } | Empty { vec: base } | ToVec { src: base, .. } => {
            expr(base, refs)?
        }
        Match { scrutinee, arms } => {
            expr(scrutinee, refs)?;
            for a in arms {
                if let Some(g) = &a.guard {
                    expr(g, refs)?;
                }
                block(&a.body, refs)?;
            }
        }
        Index { base, index, .. }
        | Get {
            vec: base, index, ..
        }
        | PtrOffset {
            ptr: base, index, ..
        } => {
            expr(base, refs)?;
            expr(index, refs)?;
        }
        Slice {
            base, start, end, ..
        }
        | StrSlice {
            base, start, end, ..
        } => {
            expr(base, refs)?;
            expr(start, refs)?;
            expr(end, refs)?;
        }
        Push { value, .. } => expr(value, refs)?,
        Borrow { projections, .. } => {
            for p in projections {
                if let TPlaceStep::Index { index, .. } = p {
                    expr(index, refs)?;
                }
            }
        }
        Deref { ptr, index, .. } => {
            expr(ptr, refs)?;
            if let Some(i) = index {
                expr(i, refs)?;
            }
        }
        PtrAddr { ptr } | PtrBank { ptr } | PtrNull { ptr } => expr(ptr, refs)?,
        Int(_)
        | Float(_)
        | Bool(_)
        | Name(..)
        | Move { .. }
        | CStr(_)
        | Str(_)
        | Char(_)
        | Clear { .. }
        | Pop { .. } => {}
    }
    Ok(())
}

/// Capability checking follows active cfg syntax; opaque DSL text is data.
pub fn syntax(ast: &mut crate::ast::Module) -> Result<(), CompileError> {
    struct Check;
    impl super::walk::Visitor for Check {
        fn expression(&mut self, expr: &mut crate::ast::Expr) -> Result<bool, CompileError> {
            if matches!(expr.kind, crate::ast::ExprKind::Unsafe(_)) {
                return Err(forbidden("unsafe code"));
            }
            Ok(true)
        }
    }
    super::walk::module(&mut Check, ast)
}
