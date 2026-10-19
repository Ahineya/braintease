//! Generic trait operations use the ordinary generic-call inference engine.
//! Signature-only contracts supply Self and method parameters, then dispatch
//! replaces each concrete call with its proven implementation before lowering.
use super::*;

pub(in crate::check) fn contract_name(tr: &str, method: &str) -> String {
    format!("$contract${tr}${method}")
}

pub(super) fn method_function(tr: &Trait, method: &crate::ast::ImplItem) -> Function {
    let mut type_params = vec![TypeParam {
        name: "Self".into(),
        arity: tr.arity,
        bounds: vec![(tr.name.clone(), tr.name_span)],
        span: tr.name_span,
    }];
    type_params.extend(method.type_params.clone());
    let owner = ParsedTy::Named {
        name: "Self".into(),
        args: Vec::new(),
        span: method.name_span,
    };
    let mut params = method_params(&owner, method);
    for param in &mut params {
        if let Some(ty) = &mut param.ty {
            qualify_impl_type(ty, &owner, &tr.name);
        }
    }
    let mut ret = method.ret.clone().unwrap_or_else(unit);
    qualify_impl_type(&mut ret, &owner, &tr.name);
    Function {
        name: contract_name(&tr.name, &method.name),
        name_span: method.name_span,
        type_params,
        params,
        ret: Some(ret),
        body: method.body.clone(),
        is_async: false,
        c_export: false,
        exported: false,
    }
}

pub(super) fn method_params(owner: &ParsedTy, method: &crate::ast::ImplItem) -> Vec<Param> {
    let mut params = Vec::new();
    if let Some(receiver) = method.receiver {
        params.push(Param {
            name: "self".into(),
            name_span: method.name_span,
            pat: Pat::Name {
                name: "self".into(),
                span: method.name_span,
            },
            ty: Some(receiver_ty(owner, receiver, method.name_span)),
        });
    }
    params.extend(method.params.clone());
    params
}

impl Checker<'_> {
    pub(in crate::check) fn validate_method_generics(
        &self,
        item: &Impl,
        have: &crate::ast::ImplItem,
        want: &crate::ast::ImplItem,
    ) -> Result<(), CompileError> {
        if have.type_params.len() != want.type_params.len()
            || have
                .type_params
                .iter()
                .zip(&want.type_params)
                .any(|(a, b)| a.arity != b.arity)
        {
            return Err(CompileError::at(
                self.source,
                have.name_span.start,
                "generic method signature does not match the trait",
            ));
        }
        for (a, b) in have.type_params.iter().zip(&want.type_params) {
            if item.type_params.iter().any(|outer| outer.name == a.name) {
                return Err(CompileError::at(
                    self.source,
                    a.span.start,
                    "method parameter shadows an impl type parameter",
                ));
            }
            let bounds = |p: &TypeParam| -> HashSet<_> {
                p.bounds
                    .iter()
                    .map(|(n, _)| self.trait_name(n).unwrap_or_else(|| n.clone()))
                    .collect()
            };
            if bounds(a) != bounds(b) {
                return Err(CompileError::at(
                    self.source,
                    a.span.start,
                    "method bounds must match the trait; strengthened bounds are not allowed",
                ));
            }
        }
        Ok(())
    }

    pub(in crate::check) fn check_operation(
        &mut self,
        span: usize,
        name: &str,
        owner: Ty,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let tr = self.traits.iter().find(|t| t.name == name).unwrap().clone();
        let required = tr
            .methods
            .iter()
            .find(|m| m.name == method)
            .ok_or_else(|| {
                CompileError::at(
                    self.source,
                    span,
                    format!("`{name}` has no method `{method}`"),
                )
            })?
            .clone();
        self.loose_receiver = required.receiver == Some(Recv::Shared);
        let contract = contract_name(name, method);
        let checked =
            self.check_generic_call_with_prefix(span, &contract, &[], args, expected, &[owner]);
        let (mut call, inferred) = checked.map_err(|error| {
            CompileError::at(
                self.source,
                error.offset.unwrap_or(span),
                error
                    .detail
                    .replace(&contract, &format!("{}::{method}", label(name))),
            )
        })?;
        if !self.opaque.contains_key(&owner) {
            let implementation = self
                .trait_impls
                .iter()
                .find(|i| {
                    i.item.trait_name.as_ref().unwrap().0 == name
                        && self.impl_args(&i.item, owner).is_some()
                })
                .ok_or_else(|| {
                    CompileError::at(
                        self.source,
                        span,
                        format!("{} does not implement `{name}`", self.show(owner)),
                    )
                })?
                .clone();
            let mut type_args = self.impl_args(&implementation.item, owner).unwrap();
            type_args.extend_from_slice(&inferred[1..]);
            let function = &implementation.methods[method];
            let target = if type_args.is_empty() {
                function.clone()
            } else {
                self.instantiate(function, &type_args)?
            };
            if let TExprKind::Call { callee, .. } = &mut call.kind {
                *callee = target;
            }
        }
        if self.editor {
            let TExprKind::Call { args, .. } = &call.kind else {
                unreachable!()
            };
            let skip = usize::from(required.receiver.is_some());
            let signature = format!(
                "fn({}) -> {}",
                args.iter()
                    .skip(skip)
                    .map(|arg| self.show(arg.ty))
                    .collect::<Vec<_>>()
                    .join(", "),
                self.show(call.ty)
            );
            self.method_targets.insert(
                (span, self.current_span.end, method.into()),
                (required.name_span, signature),
            );
        }
        Ok(call)
    }

    pub(in crate::check) fn check_constructor_operation(
        &mut self,
        span: usize,
        qualifier: &str,
        owner: Ty,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let key = (span, self.current_span.end, method.into());
        let contract = if let Some(tr) = self.trait_dispatch.get(&key) {
            tr.clone()
        } else {
            let candidates = self.trait_candidates(span, owner, method)?;
            if candidates.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    if candidates.is_empty() {
                        format!("constructor `{qualifier}` has no `{method}` operation in its bounds or visible traits")
                    } else {
                        format!(
                            "ambiguous constructor operation `{method}`: {}",
                            candidates.join(", ")
                        )
                    },
                ));
            }
            if self.opaque.contains_key(&owner) {
                self.trait_dispatch.insert(key, candidates[0].clone());
            }
            candidates[0].clone()
        };
        self.check_operation(span, &contract, owner, method, args, expected)
    }
}
