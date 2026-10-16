//! Construction, explicit transfers and temporary access to move-only owners.
use super::*;

impl Checker<'_> {
    pub(in crate::check) fn check_resource_declarations(&self) -> Result<(), CompileError> {
        let module = TModule {
            functions: self.typed.clone(),
            adts: self.adts.clone(),
            structs: self
                .structs
                .iter()
                .map(|s| s.fields.iter().map(|(_, ty)| *ty).collect())
                .collect(),
            tuples: self.tuples.clone(),
            collections: CollectionTypes {
                vectors: self.vec_elems.clone(),
                owners: self.owner_elems.clone(),
                slices: self.slice_elems.clone(),
                arrays: self.arrays.iter().map(|a| a.elem).collect(),
            },
            repr_c: self.structs.iter().map(|s| s.repr_c).collect(),
            externs: self.externs.clone(),
        };
        crate::resources::check(self.source, &module)
    }

    pub(super) fn check_storage_builtin(
        &mut self,
        span: usize,
        name: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{name} expects one argument"),
            ));
        }
        let value = self.check_expr(&args[0], None)?;
        let (ty, kind) = if name == "__erase_owned" {
            if !matches!(value.ty, Ty::Owned(_)) {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "erasure requires Owned<T>",
                ));
            }
            (Ty::ErasedOwner, TExprKind::EraseOwner(Box::new(value)))
        } else {
            self.require_unsafe(span)?;
            let Ty::Ptr(id) = value.ty else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "raw borrowing needs a mutable pointer",
                ));
            };
            let pointer = self.ptrs[id as usize].clone();
            if !pointer.mutable {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "raw borrowing needs a mutable pointer",
                ));
            }
            self.require_vec_elem(span, pointer.inner)?;
            (
                self.intern_ref(true, pointer.inner),
                TExprKind::RawBorrow(Box::new(value)),
            )
        };
        if expected.is_some_and(|t| t != ty) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{name} returns {}", self.show(ty)),
            ));
        }
        Ok(TExpr { ty, kind })
    }

    pub(super) fn check_replace(
        &mut self,
        span: usize,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if args.len() != 2 {
            return Err(CompileError::at(
                self.source,
                span,
                "replace expects a mutable reference and a value",
            ));
        }
        let place = self.check_expr(&args[0], None)?;
        let Ty::Ref(id) = place.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                "replace needs a mutable reference",
            ));
        };
        let reference = self.refs[id as usize].clone();
        if !reference.mutable {
            return Err(CompileError::at(
                self.source,
                span,
                "replace needs a mutable reference",
            ));
        }
        self.note_reference_use(span, &place, Some(place.ty))?;
        self.require_vec_elem(span, reference.inner)?;
        let value = self.check_expr(&args[1], Some(reference.inner))?;
        if value.ty != reference.inner || expected.is_some_and(|t| t != reference.inner) {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "replace needs values of type {}",
                    self.show(reference.inner)
                ),
            ));
        }
        self.note_sig(format!(
            "fn({}, {}) -> {}",
            self.show(place.ty),
            self.show(value.ty),
            self.show(value.ty)
        ));
        Ok(TExpr {
            ty: reference.inner,
            kind: TExprKind::Replace {
                place: Box::new(place),
                value: Box::new(value),
            },
        })
    }
    pub(super) fn check_into_inner(
        &mut self,
        span: usize,
        value: TExpr,
        args: &[Expr],
        inner: Ty,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if !args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                "into_inner expects no arguments",
            ));
        }
        if value.ty.is_ref() {
            return Err(CompileError::at(
                self.source,
                span,
                "into_inner consumes its owner; use move(owner).into_inner()",
            ));
        }
        if expected.is_some_and(|t| t != inner) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("into_inner returns {}", self.show(inner)),
            ));
        }
        self.note_sig(format!("fn() -> {}", self.show(inner)));
        Ok(TExpr {
            ty: inner,
            kind: TExprKind::IntoInner(Box::new(value)),
        })
    }

    pub(super) fn intern_owned(&mut self, inner: Ty) -> Ty {
        if let Some(id) = self.owner_elems.iter().position(|ty| *ty == inner) {
            return Ty::Owned(id as u32);
        }
        let id = self.owner_elems.len() as u32;
        self.owner_elems.push(inner);
        Ty::Owned(id)
    }

    pub(super) fn check_resource_builtin(
        &mut self,
        span: usize,
        name: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let arity = if name == "owned" { 2 } else { 1 };
        if args.len() != arity {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` expects {arity} arguments"),
            ));
        }
        let value = if name == "owned" {
            let want = expected.and_then(|ty| match ty {
                Ty::Owned(id) => Some(self.owner_elems[id as usize]),
                _ => None,
            });
            self.check_expr(&args[0], want)?
        } else {
            self.check_expr(&args[0], if name == "move" { expected } else { None })?
        };
        let input_ty = value.ty;
        let (ty, kind) = match name {
            "owned" => {
                self.require_vec_elem(span, value.ty)?;
                let inner = self.intern_ref(true, value.ty);
                let cleanup_ty = self.intern_func(vec![inner], Ty::Unit);
                let cleanup = self.check_expr(&args[1], Some(cleanup_ty))?;
                if cleanup.ty != cleanup_ty {
                    return Err(CompileError::at(
                        self.source,
                        args[1].span.start,
                        format!("cleanup must have type {}", self.show(cleanup_ty)),
                    ));
                }
                (
                    self.intern_owned(value.ty),
                    TExprKind::Owned {
                        value: Box::new(value),
                        cleanup: Box::new(cleanup),
                    },
                )
            }
            "move" => {
                let TExprKind::Name(binding, _) = value.kind else {
                    return Err(CompileError::at(self.source, args[0].span.start,
                        "move() needs a local name; borrow fields/elements or move their containing value"));
                };
                if value.ty.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "cannot move a borrowed reference",
                    ));
                }
                if self.captures.iter().any(|cap| cap.local == binding)
                    || self.bindings[binding as usize].shared
                {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "cannot move a closure capture",
                    ));
                }
                self.note_use(span, binding, true)?;
                (value.ty, TExprKind::Move { binding, span })
            }
            "drop" => (Ty::Unit, TExprKind::Drop(Box::new(value))),
            _ => unreachable!(),
        };
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
        let input = self.show(input_ty);
        let signature = if name == "owned" {
            format!("fn({input}, fn(&mut {input}) -> unit) -> {}", self.show(ty))
        } else {
            format!("fn({input}) -> {}", self.show(ty))
        };
        self.note_sig(signature);
        Ok(TExpr { ty, kind })
    }

    pub(super) fn check_owner_borrow(
        &mut self,
        span: usize,
        receiver: &Expr,
        recv_ty: Ty,
        owner: u32,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if !args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{method}` expects no arguments"),
            ));
        }
        let mutable = method == "borrow_mut";
        let inner = self.owner_elems[owner as usize];
        let source = if recv_ty.is_ref() {
            Expr {
                span: receiver.span,
                kind: ExprKind::Deref(Box::new(receiver.clone())),
            }
        } else {
            receiver.clone()
        };
        let mut value = self.check_borrow(span, mutable, false, &source, None)?;
        let ty = self.intern_ref(mutable, inner);
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
        let TExprKind::Borrow { projections, .. } = &mut value.kind else {
            unreachable!()
        };
        projections.push(TPlaceStep::Owned);
        value.ty = ty;
        self.note_sig(format!("fn() -> {}", self.show(ty)));
        Ok(value)
    }
}
