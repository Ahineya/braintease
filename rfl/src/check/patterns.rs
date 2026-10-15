//! Patterns in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_match(
        &mut self,
        span: usize,
        scrutinee: &Expr,
        arms: &[crate::ast::Arm],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let scrutinee = self.check_expr(scrutinee, None)?;
        match scrutinee.ty {
            Ty::Adt(_) | Ty::Bool | Ty::Char | Ty::Struct { .. } | Ty::Tuple { .. } => {}
            ty if ty.is_int() => {}
            ty => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("match expects a data type, found {}", self.show(ty)),
                ));
            }
        }
        let mut covering = Vec::new();
        let mut result: Option<Ty> = expected;
        let mut typed = Vec::new();
        for arm in arms {
            self.scopes.push(HashMap::new());
            let arm_scope = Span::new(arm.pat.span().end, arm.body.span.end);
            self.scope_spans.push(arm_scope);
            let pat = self.check_pat(&arm.pat, scrutinee.ty, false)?;
            let previous: Vec<_> = covering
                .iter()
                .map(|index: &usize| &typed[*index])
                .map(|arm: &TArm| &arm.pat)
                .collect();
            if !self.pattern_useful(&previous, Some(&pat), scrutinee.ty) {
                return Err(CompileError::at(
                    self.source,
                    arm_span(&arm.pat),
                    "unreachable pattern",
                ));
            }
            let guard = match &arm.guard {
                Some(expr) => {
                    let guard = self.check_expr(expr, Some(Ty::Bool))?;
                    if guard.ty != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            expr.span.start,
                            format!("a guard must be bool, found {}", self.show(guard.ty)),
                        ));
                    }
                    Some(guard)
                }
                None => None,
            };
            if guard.is_none() {
                covering.push(typed.len());
            }
            let body = match &arm.body.kind {
                ExprKind::Block(block) => self.check_block(block, result)?,
                _ => {
                    let expr = self.check_expr(&arm.body, result)?;
                    TBlock {
                        stmts: Vec::new(),
                        tail: Some(expr),
                    }
                }
            };
            self.pop_scope();
            self.scope_spans.pop();
            let diverges = expr_returns(&arm.body);
            let arm_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
            if arm_ty.is_ref() {
                return Err(CompileError::at(
                    self.source,
                    arm.body.span.start,
                    "a reference cannot escape this match",
                ));
            }
            if let Some(want) = result {
                if !diverges && arm_ty != want {
                    return Err(CompileError::at(
                        self.source,
                        arm.body.span.start,
                        format!(
                            "expected {}, this arm has type {}",
                            self.show(want),
                            self.show(arm_ty)
                        ),
                    ));
                }
            }
            if !diverges {
                result = Some(arm_ty);
            }
            typed.push(TArm { pat, guard, body });
        }
        let previous: Vec<_> = covering.iter().map(|index| &typed[*index].pat).collect();
        if self.pattern_useful(&previous, None, scrutinee.ty) {
            return Err(CompileError::at(self.source, span, "non-exhaustive match"));
        }
        let ty = result.unwrap_or(Ty::Unit);
        Ok(TExpr {
            ty,
            kind: TExprKind::Match {
                scrutinee: Box::new(scrutinee),
                arms: typed,
            },
        })
    }

    pub(super) fn check_pat(
        &mut self,
        pat: &Pat,
        ty: Ty,
        mutable: bool,
    ) -> Result<TPat, CompileError> {
        let mut bound = Vec::new();
        self.check_pat_at(pat, ty, mutable, &mut bound)
    }

    pub(super) fn check_pat_at(
        &mut self,
        pat: &Pat,
        ty: Ty,
        mutable: bool,
        bound: &mut Vec<String>,
    ) -> Result<TPat, CompileError> {
        match pat {
            Pat::Name { name, .. } if name == "_" => Ok(TPat::Wild),
            Pat::Name { name, span } => {
                if self.ctor_schema.contains_key(name) {
                    let (tag, fields) = self.ctor_fields(name, ty, span.start)?;
                    if !fields.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` expects {} fields", fields.len()),
                        ));
                    }
                    return Ok(TPat::Ctor {
                        tag,
                        fields: Vec::new(),
                    });
                }
                if bound.iter().any(|found| found == name) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("`{name}` is bound twice in this pattern"),
                    ));
                }
                bound.push(name.clone());
                let id = self.bind_spanned(name.clone(), ty, mutable, *span);
                if mutable && self.shared_spans.contains(&span.start) {
                    if !shareable(ty) {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{}` cannot be captured mutably", self.show(ty)),
                        ));
                    }
                    self.bindings[id as usize].shared = true;
                }
                Ok(TPat::Bind(id))
            }
            Pat::Ctor {
                name,
                span,
                fields: pats,
            } => {
                let (tag, field_tys) = self.ctor_fields(name, ty, span.start)?;
                if pats.len() != field_tys.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "`{name}` expects {} fields, found {}",
                            field_tys.len(),
                            pats.len()
                        ),
                    ));
                }
                let mut fields = Vec::new();
                for (pat, field_ty) in pats.iter().zip(field_tys) {
                    fields.push(FieldPat {
                        ty: field_ty,
                        pat: Box::new(self.check_pat_at(pat, field_ty, mutable, bound)?),
                    });
                }
                Ok(TPat::Ctor { tag, fields })
            }
            Pat::Int { value, span } => {
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found an integer pattern", self.show(ty)),
                    ));
                }
                if !ty.in_range(*value) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("{value} does not fit in {}", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(*value))
            }
            Pat::Bool { value, span } => {
                if ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a bool pattern", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(i64::from(*value)))
            }
            Pat::Char { scalar, span } => {
                if ty != Ty::Char {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a char pattern", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(i64::from(*scalar)))
            }
            Pat::Tuple { span, fields } => {
                let Ty::Tuple { id, .. } = ty else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a tuple pattern", self.show(ty)),
                    ));
                };
                let field_tys = self.tuples[id as usize].clone();
                if fields.len() != field_tys.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "expected {} fields, found {}",
                            field_tys.len(),
                            fields.len()
                        ),
                    ));
                }
                Ok(TPat::Product {
                    fields: self.product_fields(fields, &field_tys, mutable, bound)?,
                })
            }
            Pat::Struct { name, span, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let Ty::Struct { id: got, .. } = ty else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found `{name}`", self.show(ty)),
                    ));
                };
                if got != id {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found `{name}`", self.show(ty)),
                    ));
                }
                let info = self.structs[id as usize].fields.clone();
                if fields.len() != info.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "`{name}` expects {} fields, found {}",
                            info.len(),
                            fields.len()
                        ),
                    ));
                }
                let mut ordered = Vec::new();
                for (field_name, field_ty) in &info {
                    let Some((_, _, pat)) = fields.iter().find(|(name, _, _)| name == field_name)
                    else {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("missing field `{field_name}`"),
                        ));
                    };
                    ordered.push(self.check_pat_at(pat, *field_ty, mutable, bound)?);
                }
                let mut offset = 0i64;
                let mut product = Vec::new();
                for ((_, field_ty), pat) in info.iter().zip(ordered) {
                    product.push(ProductField {
                        offset,
                        ty: *field_ty,
                        pat: Box::new(pat),
                    });
                    offset += field_ty.cells();
                }
                Ok(TPat::Product { fields: product })
            }
        }
    }

    pub(super) fn product_fields(
        &mut self,
        pats: &[Pat],
        tys: &[Ty],
        mutable: bool,
        bound: &mut Vec<String>,
    ) -> Result<Vec<ProductField>, CompileError> {
        let mut fields = Vec::new();
        let mut offset = 0i64;
        for (pat, ty) in pats.iter().zip(tys) {
            fields.push(ProductField {
                offset,
                ty: *ty,
                pat: Box::new(self.check_pat_at(pat, *ty, mutable, bound)?),
            });
            offset += ty.cells();
        }
        Ok(fields)
    }

    /// A pattern is irrefutable when it cannot fail: a binding, `_`, or the only constructor of its type.
    pub(super) fn irrefutable_pat(&self, pat: &TPat, ty: Ty) -> bool {
        match pat {
            TPat::Wild | TPat::Bind(_) => true,
            TPat::Lit(_) => false,
            TPat::Product { fields } => fields
                .iter()
                .all(|field| self.irrefutable_pat(&field.pat, field.ty)),
            TPat::Ctor { tag, fields } => {
                let Ty::Adt(id) = ty else {
                    return false;
                };
                self.adts[id as usize].ctors.len() == 1
                    && *tag == 0
                    && fields
                        .iter()
                        .all(|field| self.irrefutable_pat(&field.pat, field.ty))
            }
        }
    }
}
