//! Borrows in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn place_mutable(&self, binding: u32, ty: Ty) -> bool {
        if let Ty::Ref(id) = ty {
            self.refs[id as usize].mutable
        } else {
            self.bindings[binding as usize].mutable
        }
    }

    pub(super) fn note_borrow(
        &mut self,
        span: usize,
        binding: u32,
        mutable: bool,
    ) -> Result<(), CompileError> {
        let conflict = self
            .borrows
            .iter()
            .any(|borrow| borrow.binding == binding && (mutable || borrow.mutable));
        if conflict {
            return Err(CompileError::at(self.source, span, "already borrowed"));
        }
        self.borrows.push(LiveBorrow {
            binding,
            mutable,
            depth: self.scopes.len(),
        });
        Ok(())
    }

    pub(super) fn note_use(
        &self,
        span: usize,
        binding: u32,
        mutate: bool,
    ) -> Result<(), CompileError> {
        let conflict = self
            .borrows
            .iter()
            .any(|borrow| borrow.binding == binding && (mutate || borrow.mutable));
        if conflict {
            return Err(CompileError::at(self.source, span, "already borrowed"));
        }
        Ok(())
    }

    /// Passing or binding an existing reference creates a reborrow of that
    /// reference's capability. Its owner cannot resize/replace the referent
    /// while a projected child loan is live.
    pub(super) fn note_reference_use(
        &mut self,
        span: usize,
        expr: &TExpr,
        expected: Option<Ty>,
    ) -> Result<(), CompileError> {
        if let (TExprKind::Name(binding, ..), Ty::Ref(id)) = (&expr.kind, expr.ty) {
            let mutable = match expected {
                Some(Ty::Ref(want)) => self.refs[want as usize].mutable,
                _ => self.refs[id as usize].mutable,
            };
            self.note_borrow(span, *binding, mutable)?;
        }
        Ok(())
    }

    pub(super) fn pop_scope(&mut self) {
        let depth = self.scopes.len();
        self.borrows.retain(|borrow| borrow.depth < depth);
        self.scopes.pop();
    }

    pub(super) fn check_borrow(
        &mut self,
        span: usize,
        mutable: bool,
        temporary: bool,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let mut path = Vec::new();
        let root = borrow_root(expr, &mut path);
        let ExprKind::Name(name) = &root.kind else {
            if !temporary || mutable {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a borrow needs a named place (a local, field, or indexed element)",
                ));
            }
            let value = self.check_expr(expr, None)?;
            if value.ty.is_ref() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "cannot borrow a reference",
                ));
            }
            let ty = self.intern_ref(false, value.ty);
            if expected.is_some_and(|want| want != ty) {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(expected.unwrap()),
                        self.show(ty)
                    ),
                ));
            }
            // A frame-local snapshot lives through the call and is released by
            // the same ownership machinery as an explicit local variable.
            let id = self.bind(format!("$borrow{}", self.bindings.len()), value.ty, false);
            return Ok(TExpr {
                ty,
                kind: TExprKind::Block(Box::new(TBlock {
                    stmts: vec![TStmt::Let {
                        binding: id,
                        init: value,
                    }],
                    tail: Some(TExpr {
                        ty,
                        kind: TExprKind::Borrow {
                            span,
                            binding: id,
                            mutable,
                            projections: vec![],
                        },
                    }),
                })),
            });
        };
        // Check the name normally: capture restrictions and editor uses apply
        // to place roots just as they do to ordinary reads.
        let checked = self.check_expr(root, None)?;
        let TExprKind::Name(id, ..) = checked.kind else {
            return Err(CompileError::at(
                self.source,
                span,
                "a borrow needs a local place",
            ));
        };
        let root_ty = checked.ty;
        if root_ty.is_ref() && path.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                "cannot borrow a reference",
            ));
        }
        if mutable && !self.place_mutable(id, root_ty) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        // Reserve the root while checking indices. Reads such as xs.len() are
        // legal here; writes and mutable calls cannot invalidate the address.
        let mark = self.borrows.len();
        self.note_borrow(span, id, false)?;
        let mut ty = self.referent(root_ty);
        let mut projections = Vec::new();
        for (position, projection) in path.iter().enumerate() {
            match &projection.kind {
                ExprKind::Field { name, .. } => {
                    let (offset, field) = self.field_at(projection.span.start, ty, name)?;
                    projections.push(crate::typed::TPlaceStep::Field { offset });
                    ty = field;
                }
                ExprKind::Index { index, .. } => {
                    if mutable && matches!(ty, Ty::Slice(_)) {
                        return Err(CompileError::at(self.source, span, "slices are read-only"));
                    }
                    if !matches!(ty, Ty::Vec(_) | Ty::Slice(_) | Ty::Array { .. }) {
                        return Err(CompileError::at(
                            self.source,
                            projection.span.start,
                            "only vector, slice, or array elements can be borrowed",
                        ));
                    }
                    let elem = self.element_of(ty).unwrap();
                    let index = self.check_expr(index, Some(Ty::Usize))?;
                    if index.ty != Ty::Usize {
                        return Err(CompileError::at(
                            self.source,
                            projection.span.start,
                            format!("expected usize, found {}", self.show(index.ty)),
                        ));
                    }
                    projections.push(crate::typed::TPlaceStep::Index {
                        index: Box::new(index),
                        container: ty,
                        elem,
                    });
                    ty = elem;
                }
                ExprKind::Deref(_) if position == 0 && root_ty.is_ref() => {}
                _ => {
                    return Err(CompileError::at(
                        self.source,
                        projection.span.start,
                        "a borrowed place can only dereference a reference at its root",
                    ))
                }
            }
            self.expr_types.push(ExprType {
                span: projection.span,
                ty: self.show(ty),
            });
            if self.editor {
                self.note_trait_completions(projection.span, ty);
            }
        }
        self.borrows.truncate(mark);
        let refty = self.intern_ref(mutable, ty);
        if let Some(want) = expected {
            if self.ptr_from_ref(want, refty) {
                self.require_unsafe(span)?;
                self.note_borrow(span, id, mutable)?;
                return Ok(TExpr {
                    ty: want,
                    kind: TExprKind::Borrow {
                            span,
                        binding: id,
                        mutable,
                        projections,
                    },
                });
            }
            if want != refty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(refty)),
                ));
            }
        }
        self.note_borrow(span, id, mutable)?;
        Ok(TExpr {
            ty: refty,
            kind: TExprKind::Borrow {
                            span,
                binding: id,
                mutable,
                projections,
            },
        })
    }
}

/// Returns a syntactic root and the projections in evaluation order. A call
/// result is not a place even when a field or index is selected from it.
fn borrow_root<'e>(expr: &'e Expr, path: &mut Vec<&'e Expr>) -> &'e Expr {
    let base = match &expr.kind {
        ExprKind::Field { base, .. } | ExprKind::Index { base, .. } => base,
        ExprKind::Deref(base) => base,
        _ => return expr,
    };
    let root = borrow_root(base, path);
    path.push(expr);
    root
}
