//! Ffi in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn register_externs(
        &mut self,
        decls: &[crate::ast::ExternDecl],
    ) -> Result<(), CompileError> {
        for decl in decls {
            if is_builtin(&decl.name) {
                return Err(CompileError::at(
                    self.source,
                    decl.name_span.start,
                    format!("`{}` is a builtin", decl.name),
                ));
            }
            if self.sigs.contains_key(&decl.name)
                || self.fn_schemas.contains_key(&decl.name)
                || self.ctor_schema.contains_key(&decl.name)
            {
                return Err(CompileError::at(
                    self.source,
                    decl.name_span.start,
                    format!("duplicate function `{}`", decl.name),
                ));
            }
            let mut params = Vec::new();
            for param in &decl.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                self.validate_ty(ty, &[])?;
                let ty = self.apply(ty, &HashMap::new())?;
                self.require_c_type(param.name_span.start, ty, false)?;
                params.push(ty);
            }
            let ret = match &decl.ret {
                Some(ty) => {
                    self.validate_ty(ty, &[])?;
                    let ty = self.apply(ty, &HashMap::new())?;
                    self.require_c_type(decl.name_span.start, ty, true)?;
                    ty
                }
                None => Ty::Unit,
            };
            self.externs.push(crate::typed::ExternFn {
                name: decl.name.clone(),
                params: params.clone(),
                ret,
                variadic: decl.variadic,
            });
            let ret = if ret == Ty::Unit { None } else { Some(ret) };
            self.sigs.insert(
                decl.name.clone(),
                Sig {
                    params,
                    ret,
                    c_abi: true,
                    variadic: decl.variadic,
                },
            );
            if decl.name_span.end > decl.name_span.start {
                let shown = self.sigs[&decl.name]
                    .params
                    .iter()
                    .map(|ty| self.show(*ty))
                    .collect::<Vec<_>>()
                    .join(", ");
                let ret = self.sigs[&decl.name]
                    .ret
                    .map(|ty| self.show(ty))
                    .unwrap_or_else(|| "()".to_string());
                self.facts.push(BindingFact {
                    name: decl.name.clone(),
                    def: decl.name_span,
                    ty: format!("extern \"C\" fn({shown}) -> {ret}"),
                    scope: Span::new(0, self.source.len()),
                    visible_from: 0,
                    variant: false,
                });
            }
        }
        Ok(())
    }

    pub(super) fn require_c_type(
        &self,
        span: usize,
        ty: Ty,
        is_return: bool,
    ) -> Result<(), CompileError> {
        if is_return && ty == Ty::Unit {
            return Ok(());
        }
        if ty.is_int() || ty.is_float() {
            return Ok(());
        }
        if self.is_repr_struct(ty) {
            return Ok(());
        }
        if let Ty::Ptr(id) = ty {
            let inner = self.ptrs[id as usize].inner;
            if self.c_pointee(inner) {
                return Ok(());
            }
        }
        if matches!(ty, Ty::CFn(_)) {
            return Ok(());
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("`{}` cannot be used in a C signature", self.show(ty)),
        ))
    }

    pub(super) fn is_repr_struct(&self, ty: Ty) -> bool {
        matches!(ty, Ty::Struct { id, .. } if self.structs.get(id as usize).is_some_and(|info| info.repr_c))
    }

    pub(super) fn c_pointee(&self, ty: Ty) -> bool {
        ty.is_int() || ty.is_float() || self.is_repr_struct(ty)
    }

    /// `&mut T` may become `*mut T` or `*const T`. `&T` may become `*const T`.
    pub(super) fn ptr_from_ref(&self, want: Ty, got: Ty) -> bool {
        let Ty::Ptr(want_id) = want else { return false };
        let Ty::Ref(got_id) = got else { return false };
        let want_ptr = &self.ptrs[want_id as usize];
        let got_ref = &self.refs[got_id as usize];
        want_ptr.inner == got_ref.inner && (!want_ptr.mutable || got_ref.mutable)
    }

    pub(super) fn promote_vararg(&self, span: usize, expr: TExpr) -> Result<TExpr, CompileError> {
        if expr.ty == Ty::F32 {
            return Ok(TExpr {
                ty: Ty::F64,
                kind: TExprKind::Cast {
                    expr: Box::new(expr),
                    to: Ty::F64,
                },
            });
        }
        if matches!(expr.ty, Ty::I8 | Ty::U8) {
            return Ok(TExpr {
                ty: Ty::I16,
                kind: TExprKind::Cast {
                    expr: Box::new(expr),
                    to: Ty::I16,
                },
            });
        }
        if expr.ty.is_int()
            || expr.ty.is_float()
            || matches!(expr.ty, Ty::Ptr(_))
            || self.is_repr_struct(expr.ty)
        {
            return Ok(expr);
        }
        Err(CompileError::at(
            self.source,
            span,
            format!(
                "`{}` cannot be passed to a variadic function",
                self.show(expr.ty)
            ),
        ))
    }

    pub(super) fn coerce_raw(
        &self,
        span: usize,
        expr: TExpr,
        want: Ty,
    ) -> Result<TExpr, CompileError> {
        if expr.ty == want {
            return Ok(expr);
        }
        if self.ptr_from_ref(want, expr.ty) {
            if self.unsafe_depth == 0 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "converting a reference to a raw pointer needs `unsafe`",
                ));
            }
            return Ok(TExpr {
                ty: want,
                kind: expr.kind,
            });
        }
        Ok(expr)
    }

    pub(super) fn require_unsafe(&self, span: usize) -> Result<(), CompileError> {
        if self.unsafe_depth == 0 {
            Err(CompileError::at(
                self.source,
                span,
                "a raw pointer operation needs `unsafe`",
            ))
        } else {
            Ok(())
        }
    }

    pub(super) fn plain_pointee(&self, ty: Ty) -> bool {
        if ty.is_int()
            || ty.is_float()
            || ty == Ty::Bool
            || ty == Ty::Char
            || matches!(ty, Ty::Ptr(_))
        {
            return true;
        }
        if let Ty::Struct { id, .. } = ty {
            let info = &self.structs[id as usize];
            return info.repr_c
                && info
                    .fields
                    .iter()
                    .all(|(_, field)| self.plain_pointee(*field));
        }
        if let Ty::Array { id, .. } = ty {
            return self.plain_pointee(self.arrays[id as usize].elem);
        }
        false
    }

    pub(super) fn require_pointee(&self, span: usize, ty: Ty) -> Result<(), CompileError> {
        if self.plain_pointee(ty) {
            Ok(())
        } else {
            Err(CompileError::at(
                self.source,
                span,
                format!(
                    "cannot load or store `{}` through a raw pointer",
                    self.show(ty)
                ),
            ))
        }
    }

    pub(super) fn check_deref(
        &mut self,
        span: usize,
        inner: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ptr = self.check_expr(inner, None)?;
        if ptr.ty.is_ref() {
            let ty = self.referent(ptr.ty);
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
            return Ok(TExpr {
                ty,
                kind: TExprKind::Field {
                    base: Box::new(ptr),
                    offset: 0,
                },
            });
        }
        self.check_ptr_load(span, ptr, None, expected)
    }

    pub(super) fn check_ptr_load(
        &mut self,
        span: usize,
        ptr: TExpr,
        index: Option<&Expr>,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        self.require_unsafe(span)?;
        let Ty::Ptr(id) = ptr.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected a raw pointer, found {}", self.show(ptr.ty)),
            ));
        };
        let elem = self.ptrs[id as usize].inner;
        self.require_pointee(span, elem)?;
        if let Some(want) = expected {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        let index = match index {
            Some(index) => {
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                Some(Box::new(index))
            }
            None => None,
        };
        Ok(TExpr {
            ty: elem,
            kind: TExprKind::Deref {
                ptr: Box::new(ptr),
                index,
                stride: elem.cells(),
            },
        })
    }

    pub(super) fn check_ptr_store(
        &mut self,
        ptr: &Expr,
        index: Option<&Expr>,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let span = ptr.span.start;
        let ptr = self.check_expr(ptr, None)?;
        self.store_through_ptr(span, ptr, index, value)
    }

    pub(super) fn store_through_ptr(
        &mut self,
        span: usize,
        ptr: TExpr,
        index: Option<&Expr>,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        if let Ty::Ref(id) = ptr.ty {
            let info = &self.refs[id as usize];
            if !info.mutable {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "cannot store through a shared reference",
                ));
            }
            if index.is_some() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a reference is not an indexed pointer",
                ));
            }
            let elem = info.inner;
            let TExprKind::Name(binding, ..) = ptr.kind else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a reference store needs a name",
                ));
            };
            self.note_use(span, binding, true)?;
            let value = self.check_expr(value, Some(elem))?;
            if value.ty != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(elem),
                        self.show(value.ty)
                    ),
                ));
            }
            return Ok(TStmt::FieldAssign {
                binding,
                offset: 0,
                value,
            });
        }
        self.require_unsafe(span)?;
        let Ty::Ptr(id) = ptr.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected a raw pointer, found {}", self.show(ptr.ty)),
            ));
        };
        let info = &self.ptrs[id as usize];
        if !info.mutable {
            return Err(CompileError::at(
                self.source,
                span,
                "cannot store through `*const`",
            ));
        }
        let elem = info.inner;
        self.require_pointee(span, elem)?;
        let stride = elem.cells();
        let index = match index {
            Some(index) => {
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                Some(index)
            }
            None => None,
        };
        let value = self.check_expr(value, Some(elem))?;
        if value.ty != elem {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, found {}",
                    self.show(elem),
                    self.show(value.ty)
                ),
            ));
        }
        Ok(TStmt::PtrStore {
            ptr,
            index,
            value,
            stride,
        })
    }

    pub(super) fn check_ptr_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Ty::Ptr(id) = recv.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                "expected a raw pointer",
            ));
        };
        let inner = self.ptrs[id as usize].inner;
        match method {
            "addr" | "bank" | "is_null" => {
                if !args.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`{method}` expects 0 arguments, found {}", args.len()),
                    ));
                }
                let ty = if method == "is_null" {
                    Ty::Bool
                } else {
                    Ty::U16
                };
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "expected {}, `{method}` has type {}",
                                self.show(want),
                                self.show(ty)
                            ),
                        ));
                    }
                }
                let kind = match method {
                    "addr" => TExprKind::PtrAddr {
                        ptr: Box::new(recv),
                    },
                    "bank" => TExprKind::PtrBank {
                        ptr: Box::new(recv),
                    },
                    _ => TExprKind::PtrNull {
                        ptr: Box::new(recv),
                    },
                };
                Ok(TExpr { ty, kind })
            }
            "offset" => {
                self.require_unsafe(span)?;
                if args.len() != 1 {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`offset` expects 1 argument, found {}", args.len()),
                    ));
                }
                let index = self.check_expr(&args[0], Some(Ty::I16))?;
                if index.ty != Ty::I16 {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected i16, found {}", self.show(index.ty)),
                    ));
                }
                if let Some(want) = expected {
                    if want != recv.ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(want), self.show(recv.ty)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: recv.ty,
                    kind: TExprKind::PtrOffset {
                        ptr: Box::new(recv),
                        index: Box::new(index),
                        stride: inner.cells(),
                    },
                })
            }
            _ => Err(CompileError::at(
                self.source,
                span,
                format!("a raw pointer has no method `{method}`"),
            )),
        }
    }
}
