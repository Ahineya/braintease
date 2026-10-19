//! Lexically visible trait candidates and ordinary direct-call lowering.
use super::*;
impl Checker<'_> {
    pub(in crate::check) fn impl_args(&self, item: &Impl, ty: Ty) -> Option<Vec<Ty>> {
        let params: Vec<_> = item.type_params.iter().map(|p| p.name.clone()).collect();
        let mut solved = vec![None; params.len()];
        if self
            .infer_field(&item.ty, ty, &params, &mut solved, item.span.start)
            .is_err()
        {
            return None;
        }
        solved.into_iter().collect()
    }

    pub(in crate::check) fn satisfies(&self, name: &str, ty: Ty) -> Result<bool, CompileError> {
        self.satisfies_at(name, ty, &mut HashSet::new())
    }

    fn satisfies_at(
        &self,
        name: &str,
        ty: Ty,
        stack: &mut HashSet<(String, Ty)>,
    ) -> Result<bool, CompileError> {
        if let Some((_, bounds)) = self.opaque.get(&ty) {
            return Ok(bounds.iter().any(|b| b == name));
        }
        if self.is_copy_trait(name) {
            return Ok(self.copy_value(name, ty, &mut HashSet::new()));
        }
        let key = (name.to_string(), ty);
        if !stack.insert(key.clone()) {
            return Ok(false);
        }
        let mut result = false;
        for implementation in self.trait_impls.clone() {
            if implementation.item.trait_name.as_ref().unwrap().0 != name {
                continue;
            }
            let Some(args) = self.impl_args(&implementation.item, ty) else {
                continue;
            };
            result = true;
            for (param, arg) in implementation.item.type_params.iter().zip(args) {
                for (bound, _) in &param.bounds {
                    result &= self.satisfies_at(bound, arg, stack)?;
                }
            }
            if result {
                break;
            }
        }
        stack.remove(&key);
        Ok(result)
    }

    pub(in crate::check) fn trait_candidates(
        &self,
        span: usize,
        ty: Ty,
        method: &str,
    ) -> Result<Vec<String>, CompileError> {
        let ty = self.referent(ty);
        let visible = self
            .trait_scopes
            .iter()
            .find(|s| s.span.contains(span))
            .map(|s| s.visible.clone());
        let mut candidates = Vec::new();
        for tr in self.traits.clone() {
            if visible.as_ref().is_some_and(|v| !v.contains(&tr.name)) {
                continue;
            }
            if visible.is_none()
                && tr.name == "$std$Printable"
                && self.traits.iter().any(|t| t.name == "Printable")
            {
                continue;
            }
            if tr.methods.iter().any(|m| m.name == method) && self.satisfies(&tr.name, ty)? {
                candidates.push(tr.name);
            }
        }
        Ok(candidates)
    }

    pub(in crate::check) fn trait_return_hint(&self, name: &str, method: &str) -> Option<Ty> {
        self.trait_sigs
            .get(&(name.to_string(), method.to_string()))
            .map(|s| s.ret)
    }

    pub(in crate::check) fn check_trait_method(
        &mut self,
        span: usize,
        name: &str,
        receiver: &Expr,
        recv_ty: Ty,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let tr = self.traits.iter().find(|t| t.name == name).unwrap().clone();
        let Some(required) = tr.methods.iter().find(|m| m.name == method) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` has no method `{method}`"),
            ));
        };
        let adapted = self.adapt_receiver(span, receiver, recv_ty, required.receiver)?;
        let mut call_args = vec![adapted];
        call_args.extend_from_slice(args);
        let owner = self.referent(recv_ty);
        let receiver_hint = if recv_ty.is_ref() {
            recv_ty
        } else {
            match required.receiver {
                Some(Recv::Shared) => self.intern_ref(false, owner),
                Some(Recv::Mut) => self.intern_ref(true, owner),
                _ => owner,
            }
        };
        let result = self.check_trait_assoc_with_receiver(
            span,
            name,
            method,
            &call_args,
            expected,
            Some(receiver_hint),
        )?;
        if let TExprKind::Call { args, .. } = &result.kind {
            self.noted_fn(
                &args.iter().skip(1).map(|a| a.ty).collect::<Vec<_>>(),
                result.ty,
                false,
            );
        }
        Ok(result)
    }

    pub(in crate::check) fn check_trait_assoc(
        &mut self,
        span: usize,
        name: &str,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        self.check_trait_assoc_with_receiver(span, name, method, args, expected, None)
    }

    fn check_trait_assoc_with_receiver(
        &mut self,
        span: usize,
        name: &str,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
        receiver_hint: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let tr = self.traits.iter().find(|t| t.name == name).unwrap().clone();
        let Some(required) = tr.methods.iter().find(|m| m.name == method) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` has no method `{method}`"),
            ));
        };
        if tr.arity > 0 {
            let owner = if matches!(required.params.first().and_then(|p|p.ty.as_ref()),Some(ParsedTy::Named{name,args,..}) if name == "Self" && !args.is_empty())
            {
                let mark = self.borrows.len();
                let first = args.first().ok_or_else(|| {
                    CompileError::at(
                        self.source,
                        span,
                        "cannot infer constructor; use Constructor::operation(...)",
                    )
                })?;
                let value = self.check_expr(first, None)?;
                self.borrows.truncate(mark);
                self.decompose_constructor(value.ty).map(|(c, _)| c)
            } else {
                expected.and_then(|ty| self.decompose_constructor(ty).map(|(c, _)| c))
            };
            let owner = owner.ok_or_else(|| {
                CompileError::at(
                    self.source,
                    span,
                    "cannot infer constructor; use Constructor::operation(...)",
                )
            })?;
            return self.check_operation(span, name, owner, method, args, expected);
        }
        if !required.type_params.is_empty() {
            let mark = self.borrows.len();
            let receiver = args.first().ok_or_else(|| {
                CompileError::at(self.source, span, "trait call needs a receiver")
            })?;
            let ty = self.check_expr(receiver, receiver_hint)?.ty;
            let owner = self.referent(ty);
            self.borrows.truncate(mark);
            return self.check_operation(span, name, owner, method, args, expected);
        }
        if args.len() != required.params.len() + 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}::{method}` expects {} arguments, found {}",
                    required.params.len() + 1,
                    args.len()
                ),
            ));
        }
        let recv = self.check_expr(&args[0], receiver_hint)?;
        let ty = self.referent(recv.ty);
        let recv_kind = required.receiver.unwrap();
        let wanted = match recv_kind {
            Recv::Value => ty,
            Recv::Shared => self.intern_ref(false, ty),
            Recv::Mut => self.intern_ref(true, ty),
        };
        if !self.ref_compatible(recv.ty, wanted) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}::{method}` needs {}", self.show(wanted)),
            ));
        }
        if !self.satisfies(name, ty)? {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{} does not implement `{name}`", self.show(ty)),
            ));
        }
        let signature = self.projected_method_signature(name, method, ty)?;
        self.note_reference_use(span, &recv, Some(wanted))?;
        let mut checked = vec![recv];
        for (arg, want) in args[1..].iter().zip(signature.params) {
            let got = self.check_expr(arg, Some(want))?;
            if got.ty != want {
                return Err(CompileError::at(
                    self.source,
                    arg.span.start,
                    format!("expected {}, found {}", self.show(want), self.show(got.ty)),
                ));
            }
            self.note_reference_use(arg.span.start, &got, Some(want))?;
            checked.push(got);
        }
        let ret = signature.ret;
        if expected.is_some_and(|want| want != ret) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("trait method returns {}", self.show(ret)),
            ));
        }
        self.noted_fn(
            &checked.iter().map(|a| a.ty).collect::<Vec<_>>(),
            ret,
            false,
        );
        let callee = if self.opaque.contains_key(&ty) {
            // Only used for declaration checking; these bodies are discarded.
            format!("$bound${name}${method}")
        } else {
            let implementation = self
                .trait_impls
                .iter()
                .find(|i| {
                    i.item.trait_name.as_ref().unwrap().0 == name
                        && self.impl_args(&i.item, ty).is_some()
                })
                .unwrap()
                .clone();
            let func = &implementation.methods[method];
            let type_args = self.impl_args(&implementation.item, ty).unwrap();
            if type_args.is_empty() {
                func.clone()
            } else {
                self.instantiate(func, &type_args)?
            }
        };
        if self.editor {
            self.method_targets.insert(
                (span, self.current_span.end, method.into()),
                (
                    required.name_span,
                    format!(
                        "fn({}) -> {}",
                        checked
                            .iter()
                            .skip(1)
                            .map(|a| self.show(a.ty))
                            .collect::<Vec<_>>()
                            .join(", "),
                        self.show(ret)
                    ),
                ),
            );
        }
        Ok(TExpr {
            ty: ret,
            kind: TExprKind::Call {
                callee,
                args: checked,
            },
        })
    }
}
