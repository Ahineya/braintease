//! Collections in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_array(
        &mut self,
        span: usize,
        elems: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let (expected_elem, array_len) = match expected {
            Some(Ty::Vec(id)) => (Some(self.vec_elems[id as usize]), None),
            Some(Ty::Array { id, len, .. }) => (Some(self.arrays[id as usize].elem), Some(len)),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found an array", self.show(want)),
                ));
            }
            None => (None, None),
        };
        if let Some(len) = array_len {
            if elems.len() != len as usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {len} elements, found {}", elems.len()),
                ));
            }
        }
        if elems.is_empty() {
            let Some(elem) = expected_elem else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "cannot infer the element type of an empty array",
                ));
            };
            if array_len == Some(0) {
                let ty = self.intern_array(elem, 0);
                return Ok(TExpr {
                    ty,
                    kind: TExprKind::ArrayLit {
                        elems: Vec::new(),
                        elem,
                    },
                });
            }
            return Ok(TExpr {
                ty: self.intern_vec(elem),
                kind: TExprKind::VecLit {
                    elems: Vec::new(),
                    elem,
                },
            });
        }
        let mut checked = Vec::new();
        let first = self.check_expr(&elems[0], expected_elem)?;
        let elem = first.ty;
        self.require_vec_elem(span, elem)?;
        if let Some(want) = expected_elem {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        checked.push(first);
        for expr in &elems[1..] {
            let item = self.check_expr(expr, Some(elem))?;
            if item.ty != elem {
                return Err(CompileError::at(
                    self.source,
                    expr.span.start,
                    format!("expected {}, found {}", self.show(elem), self.show(item.ty)),
                ));
            }
            checked.push(item);
        }
        if let Some(len) = array_len {
            let ty = self.intern_array(elem, len);
            return Ok(TExpr {
                ty,
                kind: TExprKind::ArrayLit {
                    elems: checked,
                    elem,
                },
            });
        }
        Ok(TExpr {
            ty: self.intern_vec(elem),
            kind: TExprKind::VecLit {
                elems: checked,
                elem,
            },
        })
    }

    /// Inline arrays and owning slices use the ordinary value-read path.
    /// Vectors already have lowering support for a referenced header.
    pub(super) fn collection_value(&self, value: TExpr) -> TExpr {
        let inner = self.referent(value.ty);
        if value.ty.is_ref() && matches!(inner, Ty::Array { .. } | Ty::Slice(_) | Ty::String) {
            TExpr {
                ty: inner,
                kind: TExprKind::Field {
                    base: Box::new(value),
                    offset: 0,
                },
            }
        } else {
            value
        }
    }

    pub(super) fn check_index(
        &mut self,
        span: usize,
        base: &Expr,
        index: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        let base = self.collection_value(base);
        if let Ty::Ptr(_) = base.ty {
            return self.check_ptr_load(span, base, Some(index), expected);
        }
        let Some(elem) = self.element_of(base.ty) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected a vector, slice, or array, found {}",
                    self.show(base.ty)
                ),
            ));
        };
        if let Some(want) = expected {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        let index = self.check_expr(index, Some(Ty::Usize))?;
        if index.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected usize, found {}", self.show(index.ty)),
            ));
        }
        Ok(TExpr {
            ty: elem,
            kind: TExprKind::Index {
                base: Box::new(base),
                index: Box::new(index),
                elem,
            },
        })
    }

    pub(super) fn check_slice(
        &mut self,
        span: usize,
        base: &Expr,
        start: &Expr,
        end: &Expr,
        inclusive: bool,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        let base = self.collection_value(base);
        if base.ty == Ty::String {
            let start = self.check_expr(start, Some(Ty::Usize))?;
            let end = self.check_expr(end, Some(Ty::Usize))?;
            if start.ty != Ty::Usize || end.ty != Ty::Usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a slice bound must be usize",
                ));
            }
            if let Some(want) = expected {
                if want != Ty::String {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found String", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::String,
                kind: TExprKind::StrSlice {
                    base: Box::new(base),
                    start: Box::new(start),
                    end: Box::new(end),
                    inclusive,
                },
            });
        }
        let Some(elem) = self.element_of(base.ty) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected a vector, slice, or array, found {}",
                    self.show(base.ty)
                ),
            ));
        };
        let start = self.check_expr(start, Some(Ty::Usize))?;
        let end = self.check_expr(end, Some(Ty::Usize))?;
        if start.ty != Ty::Usize || end.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                "a slice bound must be usize",
            ));
        }
        let ty = self.intern_slice(elem);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(ty)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Slice {
                base: Box::new(base),
                start: Box::new(start),
                end: Box::new(end),
                inclusive,
                elem,
            },
        })
    }

    /// Element type of a vector, slice, array, or a reference to a vector.
    pub(super) fn element_of(&self, ty: Ty) -> Option<Ty> {
        match ty {
            Ty::Vec(id) => Some(self.vec_elems[id as usize]),
            Ty::Slice(id) => Some(self.slice_elems[id as usize]),
            Ty::Array { id, .. } => Some(self.arrays[id as usize].elem),
            Ty::String => Some(Ty::Char),
            Ty::Ref(id) => match self.refs[id as usize].inner {
                Ty::Vec(elem) => Some(self.vec_elems[elem as usize]),
                _ => None,
            },
            _ => None,
        }
    }

    pub(super) fn check_view_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        elem: Ty,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if method == "to_vec" {
            self.note_sig(format!("fn() -> Vec<{}>", self.show(elem)));
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`to_vec` expects 0 arguments, found {}", args.len()),
                ));
            }
            let ty = self.intern_vec(elem);
            if let Some(want) = expected {
                if want != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `to_vec` has type {}",
                            self.show(want),
                            self.show(ty)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty,
                kind: TExprKind::ToVec {
                    src: Box::new(recv),
                    elem,
                },
            });
        }
        self.check_vec_method(span, recv, method, args, elem, expected)
    }

    pub(super) fn check_vec_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        elem: Ty,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if method == "len" {
            self.note_sig("fn() -> usize".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`len` expects 0 arguments, found {}", args.len()),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `len` has type usize", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Usize,
                kind: TExprKind::Len {
                    vec: Box::new(recv),
                },
            });
        }
        if method == "is_empty" {
            self.note_sig("fn() -> bool".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`is_empty` expects 0 arguments, found {}", args.len()),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `is_empty` has type bool", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Bool,
                kind: TExprKind::Empty {
                    vec: Box::new(recv),
                },
            });
        }
        if method == "clear" {
            self.note_sig("fn() -> ()".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`clear` expects 0 arguments, found {}", args.len()),
                ));
            }
            let (binding, offset) = self.vec_place(span, &recv, "clear", true)?;
            if let Some(want) = expected {
                if want != Ty::Unit {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `clear` has type unit", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::Clear {
                    binding,
                    offset,
                    elem,
                },
            });
        }
        if method == "pop" {
            self.note_sig(format!("fn() -> Option<{}>", self.show(elem)));
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`pop` expects 0 arguments, found {}", args.len()),
                ));
            }
            let (binding, offset) = self.vec_place(span, &recv, "pop", true)?;
            let (option, some_tag, none_tag) = self.option_of(elem)?;
            if let Some(want) = expected {
                if want != option {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `pop` has type {}",
                            self.show(want),
                            self.show(option)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty: option,
                kind: TExprKind::Pop {
                    binding,
                    offset,
                    elem,
                    some_tag,
                    none_tag,
                },
            });
        }
        if method == "get" {
            self.note_sig(format!("fn(usize) -> Option<{}>", self.show(elem)));
            if args.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`get` expects 1 argument, found {}", args.len()),
                ));
            }
            let index = self.check_expr(&args[0], Some(Ty::Usize))?;
            if index.ty != Ty::Usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected usize, found {}", self.show(index.ty)),
                ));
            }
            let (option, some_tag, none_tag) = self.option_of(elem)?;
            if let Some(want) = expected {
                if want != option {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `get` has type {}",
                            self.show(want),
                            self.show(option)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty: option,
                kind: TExprKind::Get {
                    vec: Box::new(recv),
                    index: Box::new(index),
                    elem,
                    some_tag,
                    none_tag,
                },
            });
        }
        self.note_sig(format!("fn({}) -> ()", self.show(elem)));
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`push` expects 1 argument, found {}", args.len()),
            ));
        }
        let (binding, offset) = self.vec_place(span, &recv, "push", true)?;
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `push` has type unit", self.show(want)),
                ));
            }
        }
        let value = self.check_expr(&args[0], Some(elem))?;
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
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Push {
                binding,
                offset,
                value: Box::new(value),
                elem,
            },
        })
    }

    pub(super) fn option_of(&mut self, elem: Ty) -> Result<(Ty, u16, u16), CompileError> {
        let schema = self
            .schemas
            .iter()
            .position(|schema| schema.name == "Option")
            .expect("prelude Option");
        let id = self.monomorphize(schema, &[elem])?;
        let none = self.ctor_schema["None"].1;
        let some = self.ctor_schema["Some"].1;
        Ok((Ty::Adt(id), some, none))
    }

    pub(super) fn vec_place(
        &mut self,
        span: usize,
        recv: &TExpr,
        method: &str,
        mutate: bool,
    ) -> Result<(u32, i64), CompileError> {
        let mut offset = 0i64;
        let mut cursor = recv;
        let binding = loop {
            match &cursor.kind {
                TExprKind::Name(binding, ..) => break *binding,
                TExprKind::Field { base, offset: at } => {
                    offset += *at;
                    cursor = base;
                }
                _ => {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`{method}` needs a name"),
                    ));
                }
            }
        };
        let root_ty = self.bindings[binding as usize].ty;
        if !matches!(self.referent(recv.ty), Ty::Vec(_)) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{method}` needs a vector"),
            ));
        }
        if mutate && !self.place_mutable(binding, root_ty) {
            let message = if root_ty.is_ref() {
                format!("`{method}` needs a mutable reference")
            } else {
                format!("`{method}` needs a mutable name")
            };
            return Err(CompileError::at(self.source, span, message));
        }
        if mutate {
            self.note_use(span, binding, true)?;
        }
        Ok((binding, offset))
    }
}
