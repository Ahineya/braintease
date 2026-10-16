//! Methods in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_method(
        &mut self,
        span: usize,
        receiver: &Expr,
        method: &str,
        resolved: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let receiver_mark = self.borrows.len();
        let recv = self.check_expr(receiver, None)?;
        if let Some(trait_name) = self
            .trait_dispatch
            .get(&(span, self.current_span.end, method.into()))
            .cloned()
        {
            self.borrows.truncate(receiver_mark);
            return self.check_trait_method(
                span,
                &trait_name,
                receiver,
                recv.ty,
                method,
                args,
                expected,
            );
        }
        if let Ty::Owned(id) = self.referent(recv.ty) {
            if method == "into_inner" {
                return self.check_into_inner(
                    span,
                    recv,
                    args,
                    self.owner_elems[id as usize],
                    expected,
                );
            }
            if matches!(method, "borrow" | "borrow_mut") {
                self.borrows.truncate(receiver_mark);
                return self
                    .check_owner_borrow(span, receiver, recv.ty, id, method, args, expected);
            }
        }
        if let Ty::Ptr(_) = recv.ty {
            return self.check_ptr_method(span, recv, method, args, expected);
        }
        if self.referent(recv.ty) == Ty::String && matches!(method, "len" | "is_empty") {
            let recv = if recv.ty.is_ref() {
                TExpr {
                    ty: Ty::String,
                    kind: TExprKind::Field {
                        base: Box::new(recv),
                        offset: 0,
                    },
                }
            } else {
                recv
            };
            return self.check_vec_method(span, recv, method, args, Ty::U8, expected);
        }
        let place = self.referent(recv.ty);
        if let Ty::Vec(id) = place {
            let elem = self.vec_elems[id as usize];
            if matches!(
                method,
                "len" | "push" | "is_empty" | "clear" | "pop" | "get"
            ) {
                return self.check_vec_method(span, recv, method, args, elem, expected);
            }
        }
        if let Ty::Slice(id) = place {
            let elem = self.slice_elems[id as usize];
            if matches!(method, "len" | "is_empty" | "get" | "to_vec") {
                let recv = self.collection_value(recv);
                return self.check_view_method(span, recv, method, args, elem, expected);
            }
            if matches!(method, "push" | "pop" | "clear") {
                return Err(CompileError::at(self.source, span, "slices are read-only"));
            }
        }
        if let Ty::Array { id, .. } = place {
            let elem = self.arrays[id as usize].elem;
            if matches!(method, "len" | "is_empty" | "get" | "to_vec") {
                let recv = self.collection_value(recv);
                return self.check_view_method(span, recv, method, args, elem, expected);
            }
        }
        // User methods check their adapted receiver as the first argument.
        // Discard the discovery pass's loans before that actual check.
        self.borrows.truncate(receiver_mark);
        if let Some(key) = self.type_key(recv.ty) {
            if let Some(info) = self
                .methods
                .iter()
                .find(|item| item.type_name == key && item.method == method)
                .cloned()
            {
                if info.receiver.is_none() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`{method}` is an associated function of `{key}`"),
                    ));
                }
                return self.check_inherent(span, receiver, recv.ty, &info, args, expected);
            }
        }
        let candidates = self.trait_candidates(span, recv.ty, method)?;
        if candidates.len() > 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("ambiguous method `{method}`: {}", candidates.join(", ")),
            ));
        }
        if let Some(trait_name) = candidates.first() {
            if self.generic_check && self.opaque.contains_key(&self.referent(recv.ty)) {
                self.trait_dispatch.insert(
                    (span, self.current_span.end, method.into()),
                    trait_name.clone(),
                );
            }
            return self
                .check_trait_method(span, trait_name, receiver, recv.ty, method, args, expected);
        }
        let mut all = vec![receiver.clone()];
        all.extend(args.iter().cloned());
        let result = self.check_call(span, resolved, &[], &all, expected);
        if self.editor {
            if let Some(target) = self.call_def(resolved) {
                self.method_targets
                    .insert((span, self.current_span.end, method.into()), target);
            }
        }
        result
    }

    pub(super) fn check_inherent(
        &mut self,
        span: usize,
        receiver: &Expr,
        recv_ty: Ty,
        info: &Inherent,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let adapted = self.adapt_receiver(span, receiver, recv_ty, info.receiver)?;
        let mut all = vec![adapted];
        all.extend(args.iter().cloned());
        let arity = self
            .ast_fns
            .iter()
            .find(|func| func.name == info.func)
            .map(|func| func.params.len())
            .unwrap_or(0);
        let expect = arity - usize::from(info.receiver.is_some());
        if let Some(preview) = self
            .ast_fns
            .iter()
            .find(|func| func.name == info.func)
            .map(|func| parsed_signature(func, info.receiver.is_some()))
        {
            self.note_sig(preview);
        }
        if args.len() != expect {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{}` expects {expect} arguments, found {}",
                    info.method,
                    args.len()
                ),
            ));
        }
        self.loose_receiver = matches!(info.receiver, Some(Recv::Shared));
        self.sig_skip_self = true;
        let result = if self.fn_schemas.contains_key(&info.func) {
            self.check_generic_call(span, &info.func, &[], &all, expected)
        } else {
            self.check_call(span, &info.func, &[], &all, expected)
        };
        self.loose_receiver = false;
        self.sig_skip_self = false;
        self.note_inherent_target(span, &info.func, &info.method);
        result
    }

    pub(super) fn adapt_receiver(
        &self,
        span: usize,
        expr: &Expr,
        ty: Ty,
        recv: Option<Recv>,
    ) -> Result<Expr, CompileError> {
        let Some(recv) = recv else {
            return Ok(expr.clone());
        };
        match recv {
            Recv::Value => {
                if ty.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "this method takes `self` by value",
                    ));
                }
                Ok(expr.clone())
            }
            Recv::Shared => Ok(if ty.is_ref() {
                expr.clone()
            } else {
                Expr {
                    span: expr.span,
                    kind: ExprKind::Borrow {
                        mutable: false,
                        temporary: true,
                        expr: Box::new(expr.clone()),
                    },
                }
            }),
            Recv::Mut => {
                if let Ty::Ref(id) = ty {
                    if !self.refs[id as usize].mutable {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "this method needs a mutable reference",
                        ));
                    }
                    return Ok(expr.clone());
                }
                Ok(Expr {
                    span: expr.span,
                    kind: ExprKind::Borrow {
                        mutable: true,
                        temporary: true,
                        expr: Box::new(expr.clone()),
                    },
                })
            }
        }
    }

    pub(super) fn type_key(&self, ty: Ty) -> Option<String> {
        let ty = self.referent(ty);
        if self.opaque.contains_key(&ty) {
            return None;
        }
        Some(match ty {
            Ty::Adt(id) => self.schemas[self.adts[id as usize].schema].name.clone(),
            Ty::Vec(_) => "Vec".to_string(),
            Ty::Owned(_) => "Owned".to_string(),
            Ty::ErasedOwner => "ErasedOwner".to_string(),
            Ty::Slice(_) => "Slice".to_string(),
            Ty::I8 => "i8".to_string(),
            Ty::U8 => "u8".to_string(),
            Ty::I16 => "i16".to_string(),
            Ty::U16 => "u16".to_string(),
            Ty::I32 => "i32".to_string(),
            Ty::U32 => "u32".to_string(),
            Ty::I64 => "i64".to_string(),
            Ty::U64 => "u64".to_string(),
            Ty::F32 => "f32".to_string(),
            Ty::F64 => "f64".to_string(),
            Ty::Usize => "usize".to_string(),
            Ty::Bool => "bool".to_string(),
            Ty::Char => "char".to_string(),
            Ty::String => "String".to_string(),
            Ty::Struct { id, .. } => self.structs[id as usize].name.clone(),
            Ty::Unit
            | Ty::Func(_)
            | Ty::CFn(_)
            | Ty::Ref(_)
            | Ty::Ptr(_)
            | Ty::Array { .. }
            | Ty::Tuple { .. } => return None,
        })
    }

    pub(super) fn ref_compatible(&self, actual: Ty, want: Ty) -> bool {
        if actual == want {
            return true;
        }
        let (Ty::Ref(actual_id), Ty::Ref(want_id)) = (actual, want) else {
            return false;
        };
        let actual = &self.refs[actual_id as usize];
        let want = &self.refs[want_id as usize];
        actual.inner == want.inner && actual.mutable && !want.mutable
    }

    pub(super) fn check_assoc(
        &mut self,
        span: usize,
        ty_name: &str,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if let Some(trait_name) = self.trait_name(ty_name) {
            return self.check_trait_assoc(span, &trait_name, method, args, expected);
        }
        let Some(info) = self
            .methods
            .iter()
            .find(|item| item.type_name == ty_name && item.method == method)
            .cloned()
        else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{ty_name}` has no function `{method}`"),
            ));
        };
        let result = if self.fn_schemas.contains_key(&info.func) {
            self.check_generic_call(span, &info.func, &[], args, expected)
        } else {
            self.check_call(span, &info.func, &[], args, expected)
        };
        self.note_inherent_target(span, &info.func, &info.method);
        result
    }
}
