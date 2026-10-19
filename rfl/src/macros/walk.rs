//! Structural AST traversal shared by expansion and source/hygiene mapping.
use crate::{ast::*, CompileError};
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Value,
    Call,
    Type,
    Binding,
    Pattern,
    Item,
    Member,
}
pub trait Visitor {
    fn name(&mut self, _name: &mut String, _span: Span, _role: Role) -> Result<(), CompileError> {
        Ok(())
    }
    fn span(&mut self, _span: &mut Span) {}
    fn expression(&mut self, _expr: &mut Expr) -> Result<bool, CompileError> {
        Ok(true)
    }
    fn after_expression(&mut self, _expr: &Expr) -> Result<(), CompileError> {
        Ok(())
    }
}
fn name(
    v: &mut impl Visitor,
    n: &mut String,
    s: &mut Span,
    role: Role,
) -> Result<(), CompileError> {
    v.name(n, *s, role)?;
    v.span(s);
    Ok(())
}
pub fn module(v: &mut impl Visitor, m: &mut Module) -> Result<(), CompileError> {
    for a in &mut m.adts {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        params(v, &mut a.params)?;
        for c in &mut a.ctors {
            name(v, &mut c.name, &mut c.name_span, Role::Item)?;
            for t in &mut c.fields {
                ty(v, t)?;
            }
        }
    }
    for a in &mut m.structs {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        for f in &mut a.fields {
            v.span(&mut f.name_span);
            ty(v, &mut f.ty)?;
        }
    }
    for a in &mut m.consts {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        ty(v, &mut a.ty)?;
        expression(v, &mut a.init)?;
    }
    for a in &mut m.functions {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        function(
            v,
            &mut a.type_params,
            &mut a.params,
            &mut a.ret,
            &mut a.body,
        )?;
    }
    for a in &mut m.externs {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        arguments(v, &mut a.params)?;
        if let Some(t) = &mut a.ret {
            ty(v, t)?;
        }
    }
    for a in &mut m.impls {
        v.span(&mut a.span);
        params(v, &mut a.type_params)?;
        ty(v, &mut a.ty)?;
        if let Some((n, s)) = &mut a.trait_name {
            name(v, n, s, Role::Type)?;
        }
        for t in &mut a.associated_types {
            associated(v, t)?;
        }
        for f in &mut a.items {
            v.span(&mut f.name_span);
            function(
                v,
                &mut f.type_params,
                &mut f.params,
                &mut f.ret,
                &mut f.body,
            )?;
        }
    }
    for a in &mut m.traits {
        name(v, &mut a.name, &mut a.name_span, Role::Item)?;
        v.span(&mut a.span);
        for t in &mut a.associated_types {
            associated(v, t)?;
        }
        for f in &mut a.methods {
            v.span(&mut f.name_span);
            function(
                v,
                &mut f.type_params,
                &mut f.params,
                &mut f.ret,
                &mut f.body,
            )?;
        }
    }
    Ok(())
}
fn associated(v: &mut impl Visitor, a: &mut AssociatedType) -> Result<(), CompileError> {
    v.span(&mut a.name_span);
    for (n, s) in &mut a.bounds {
        name(v, n, s, Role::Type)?;
    }
    if let Some(t) = &mut a.value {
        ty(v, t)?;
    }
    Ok(())
}
fn params(v: &mut impl Visitor, ps: &mut [TypeParam]) -> Result<(), CompileError> {
    for p in ps {
        name(v, &mut p.name, &mut p.span, Role::Binding)?;
        for (n, s) in &mut p.bounds {
            name(v, n, s, Role::Type)?;
        }
    }
    Ok(())
}
fn arguments(v: &mut impl Visitor, ps: &mut [Param]) -> Result<(), CompileError> {
    for p in ps {
        name(v, &mut p.name, &mut p.name_span, Role::Binding)?;
        pattern(v, &mut p.pat)?;
        if let Some(t) = &mut p.ty {
            ty(v, t)?;
        }
    }
    Ok(())
}
fn function(
    v: &mut impl Visitor,
    ts: &mut [TypeParam],
    ps: &mut [Param],
    ret: &mut Option<ParsedTy>,
    body: &mut Block,
) -> Result<(), CompileError> {
    params(v, ts)?;
    arguments(v, ps)?;
    if let Some(t) = ret {
        ty(v, t)?;
    }
    block(v, body)
}
pub fn block(v: &mut impl Visitor, b: &mut Block) -> Result<(), CompileError> {
    v.span(&mut b.span);
    for s in &mut b.stmts {
        match s {
            Stmt::Let {
                span,
                pattern: p,
                ty: t,
                init,
                ..
            } => {
                v.span(span);
                pattern(v, p)?;
                if let Some(t) = t {
                    ty(v, t)?;
                }
                expression(v, init)?;
            }
            Stmt::Assign {
                name: n,
                name_span: s,
                value,
            }
            | Stmt::FieldAssign {
                name: n,
                name_span: s,
                value,
                ..
            } => {
                name(v, n, s, Role::Value)?;
                expression(v, value)?;
            }
            Stmt::IndexAssign {
                name: n,
                name_span: s,
                index,
                value,
                ..
            } => {
                name(v, n, s, Role::Value)?;
                expression(v, index)?;
                expression(v, value)?;
            }
            Stmt::DerefAssign { ptr, value } => {
                expression(v, ptr)?;
                expression(v, value)?;
            }
            Stmt::Expr(e) => expression(v, e)?,
            Stmt::Return { span, value } => {
                v.span(span);
                if let Some(e) = value {
                    expression(v, e)?;
                }
            }
            Stmt::Break { span } | Stmt::Continue { span } => v.span(span),
        }
    }
    if let Some(e) = &mut b.tail {
        expression(v, e)?;
    }
    Ok(())
}
pub fn expression(v: &mut impl Visitor, e: &mut Expr) -> Result<(), CompileError> {
    if !v.expression(e)? {
        return Ok(());
    }
    let start = e.span;
    match &mut e.kind {
        ExprKind::Name(n) => v.name(n, start, Role::Value)?,
        ExprKind::Macro(_) => {}
        ExprKind::Int(_)
        | ExprKind::Float(_)
        | ExprKind::Bool(_)
        | ExprKind::Char(_)
        | ExprKind::Str(_)
        | ExprKind::CStr(_) => {}
        ExprKind::Await(x)
        | ExprKind::Try(x)
        | ExprKind::Deref(x)
        | ExprKind::Unary { expr: x, .. }
        | ExprKind::Borrow { expr: x, .. } => expression(v, x)?,
        ExprKind::Binary { lhs, rhs, .. }
        | ExprKind::Range {
            start: lhs,
            end: rhs,
            ..
        } => {
            expression(v, lhs)?;
            expression(v, rhs)?;
        }
        ExprKind::Call {
            callee,
            callee_span,
            type_args,
            args,
        } => {
            name(v, callee, callee_span, Role::Call)?;
            for t in type_args {
                ty(v, t)?;
            }
            for a in args {
                expression(v, a)?;
            }
        }
        ExprKind::Assoc { ty_name, args, .. } => {
            v.name(ty_name, start, Role::Type)?;
            for a in args {
                expression(v, a)?;
            }
        }
        ExprKind::Method {
            receiver,
            args,
            method_span,
            ..
        } => {
            v.span(method_span);
            expression(v, receiver)?;
            for a in args {
                expression(v, a)?;
            }
        }
        ExprKind::If {
            cond,
            then_body,
            else_body,
        } => {
            expression(v, cond)?;
            block(v, then_body)?;
            if let Some(x) = else_body {
                expression(v, x)?;
            }
        }
        ExprKind::Block(b) | ExprKind::Unsafe(b) => block(v, b)?,
        ExprKind::Match { scrutinee, arms } => {
            expression(v, scrutinee)?;
            for a in arms {
                pattern(v, &mut a.pat)?;
                if let Some(x) = &mut a.guard {
                    expression(v, x)?;
                }
                expression(v, &mut a.body)?;
            }
        }
        ExprKind::While { cond, body } => {
            expression(v, cond)?;
            block(v, body)?;
        }
        ExprKind::For { pat, iter, body } => {
            pattern(v, pat)?;
            expression(v, iter)?;
            block(v, body)?;
        }
        ExprKind::Array(xs) | ExprKind::Tuple(xs) => {
            for x in xs {
                expression(v, x)?;
            }
        }
        ExprKind::StructLit { name: n, fields } => {
            v.name(n, start, Role::Type)?;
            for (_, x) in fields {
                expression(v, x)?;
            }
        }
        ExprKind::Field {
            base, name_span, ..
        } => {
            v.span(name_span);
            expression(v, base)?;
        }
        ExprKind::Cast { expr, ty: t } => {
            expression(v, expr)?;
            ty(v, t)?;
        }
        ExprKind::Lambda { params, body, .. } => {
            arguments(v, params)?;
            expression(v, body)?;
        }
        ExprKind::Index { base, index } => {
            expression(v, base)?;
            expression(v, index)?;
        }
        ExprKind::Slice {
            base, start, end, ..
        } => {
            expression(v, base)?;
            expression(v, start)?;
            expression(v, end)?;
        }
    }
    v.after_expression(e)?;
    v.span(&mut e.span);
    Ok(())
}
fn pattern(v: &mut impl Visitor, p: &mut Pat) -> Result<(), CompileError> {
    match p {
        Pat::Name { name: n, span: s } => name(v, n, s, Role::Pattern)?,
        Pat::Ctor {
            name: n,
            span: s,
            fields,
        } => {
            name(v, n, s, Role::Value)?;
            for f in fields {
                pattern(v, f)?;
            }
        }
        Pat::Tuple { span, fields } => {
            v.span(span);
            for f in fields {
                pattern(v, f)?;
            }
        }
        Pat::Struct {
            name: n,
            span: s,
            fields,
        } => {
            name(v, n, s, Role::Type)?;
            for (_, s, p) in fields {
                v.span(s);
                pattern(v, p)?;
            }
        }
        Pat::Int { span, .. } | Pat::Bool { span, .. } | Pat::Char { span, .. } => v.span(span),
    }
    Ok(())
}
fn ty(v: &mut impl Visitor, t: &mut ParsedTy) -> Result<(), CompileError> {
    match t {
        ParsedTy::Named {
            span,
            name: n,
            args,
        } => {
            name(v, n, span, Role::Type)?;
            for t in args {
                ty(v, t)?;
            }
        }
        ParsedTy::Projection {
            span,
            base,
            trait_name,
            name_span,
            ..
        } => {
            v.span(span);
            v.span(name_span);
            ty(v, base)?;
            if let Some((n, s)) = trait_name {
                name(v, n, s, Role::Type)?;
            }
        }
        ParsedTy::Func {
            span, params, ret, ..
        }
        | ParsedTy::CFn { span, params, ret } => {
            v.span(span);
            for t in params {
                ty(v, t)?;
            }
            ty(v, ret)?;
        }
        ParsedTy::Ref { span, inner, .. } | ParsedTy::Ptr { span, inner, .. } => {
            v.span(span);
            ty(v, inner)?;
        }
        ParsedTy::Array { span, elem, len } => {
            v.span(span);
            ty(v, elem)?;
            expression(v, len)?;
        }
        ParsedTy::Tuple { span, fields } => {
            v.span(span);
            for t in fields {
                ty(v, t)?;
            }
        }
    }
    Ok(())
}
