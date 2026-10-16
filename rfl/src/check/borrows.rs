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
        let ExprKind::Name(name) = &expr.kind else {
            if !temporary {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "only a name can be borrowed",
                ));
            }
            if mutable {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a mutable borrow needs a name",
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
            // Even when an automatic receiver uses a snapshot, protect its
            // named source from writes by the remaining call arguments.
            let mut root = expr;
            while let ExprKind::Field { base, .. } | ExprKind::Index { base, .. } = &root.kind {
                root = base;
            }
            if let ExprKind::Name(name) = &root.kind {
                if let Some(binding) = self.resolve(name) {
                    self.note_borrow(span, binding, false)?;
                }
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
                        kind: TExprKind::Borrow { binding: id },
                    }),
                })),
            });
        };
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let ty = self.bindings[id as usize].ty;
        if ty.is_ref() {
            return Err(CompileError::at(
                self.source,
                span,
                "cannot borrow a reference",
            ));
        }
        if mutable && !self.bindings[id as usize].mutable {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        let refty = self.intern_ref(mutable, ty);
        if let Some(want) = expected {
            if self.ptr_from_ref(want, refty) {
                self.require_unsafe(span)?;
                self.note_borrow(span, id, mutable)?;
                return Ok(TExpr {
                    ty: want,
                    kind: TExprKind::Borrow { binding: id },
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
            kind: TExprKind::Borrow { binding: id },
        })
    }
}
