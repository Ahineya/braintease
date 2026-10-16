//! Trait registration, coherence, and static dispatch. No trait metadata reaches IR.
use super::*;
mod coherence;
mod dispatch;
mod generics;
use super::projections::{contains_projection, depends_on_self, qualify_impl_type};
use coherence::overlaps;

#[derive(Clone)]
pub(super) struct TraitImpl {
    pub item: Impl,
    pub methods: HashMap<String, String>,
}

impl Checker<'_> {
    pub(super) fn trait_name(&self, name: &str) -> Option<String> {
        self.traits
            .iter()
            .find(|t| t.name == name)
            .or_else(|| {
                if name == "Printable" {
                    self.traits.iter().find(|t| t.name == "$std$Printable")
                } else {
                    None
                }
            })
            .map(|t| t.name.clone())
    }

    pub(super) fn prepare_traits(&mut self, module: &mut Module) -> Result<(), CompileError> {
        self.traits = module.traits.clone();
        for func in &mut module.functions {
            for p in &mut func.type_params {
                for (bound, _) in &mut p.bounds {
                    if let Some(name) = self.trait_name(bound) {
                        *bound = name;
                    }
                }
            }
        }
        for item in &mut module.impls {
            if let Some((bound, _)) = &mut item.trait_name {
                if let Some(name) = self.trait_name(bound) {
                    *bound = name;
                }
            }
            for p in &mut item.type_params {
                for (bound, _) in &mut p.bounds {
                    if let Some(name) = self.trait_name(bound) {
                        *bound = name;
                    }
                }
            }
        }
        for (i, tr) in self.traits.iter().enumerate() {
            if self.traits[..i].iter().any(|t| t.name == tr.name)
                || module.adts.iter().any(|t| t.name == tr.name)
                || module.structs.iter().any(|t| t.name == tr.name)
                || Ty::parse_name(&tr.name).is_some()
                || tr.name == "Owned"
            {
                return Err(CompileError::at(
                    self.source,
                    tr.name_span.start,
                    format!("duplicate trait or type `{}`", tr.name),
                ));
            }
            let written = self.source.get(tr.name_span.start..tr.name_span.end);
            if let Some(written) = written.filter(|s| !s.is_empty()) {
                self.facts.push(BindingFact {
                    name: written.into(),
                    def: tr.name_span,
                    ty: format!("trait {written}"),
                    scope: Span::new(0, self.source.len()),
                    visible_from: 0,
                    variant: false,
                });
            }
            self.validate_associated_declarations(tr)?;
            let mut names = HashSet::new();
            for method in &tr.methods {
                self.facts.push(BindingFact {
                    name: method.name.clone(),
                    def: method.name_span,
                    ty: trait_method_signature(method),
                    scope: method.name_span,
                    visible_from: method.name_span.start,
                    variant: false,
                });
                if !names.insert(&method.name) {
                    return Err(CompileError::at(
                        self.source,
                        method.name_span.start,
                        format!("duplicate trait method `{}`", method.name),
                    ));
                }
            }
        }
        for (index, item) in module.impls.iter().enumerate() {
            let Some((name, span)) = &item.trait_name else {
                continue;
            };
            let Some(tr) = self.traits.iter().find(|t| t.name == *name).cloned() else {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    format!("unknown trait `{name}`"),
                ));
            };
            self.uses.push(NameUse {
                span: *span,
                def: tr.name_span,
                def_file: None,
                ty: format!("trait {}", label(name)),
            });
            if contains_projection(&item.ty) {
                return Err(CompileError::at(
                    self.source,
                    item.ty.span().start,
                    "associated types are not allowed in impl type patterns",
                ));
            }
            self.validate_associated_definitions(&tr, item)?;
            let ParsedTy::Named { name: outer, .. } = &item.ty else {
                return Err(CompileError::at(
                    self.source,
                    item.span.start,
                    "a trait impl needs a nominal type",
                ));
            };
            if item.type_params.iter().any(|p| p.name == *outer) {
                return Err(CompileError::at(
                    self.source,
                    item.span.start,
                    "a trait impl needs a nominal outer type",
                ));
            }
            let mut used = Vec::new();
            collect_parameters(&item.ty, &mut used);
            if let Some(p) = item.type_params.iter().find(|p| !used.contains(&p.name)) {
                return Err(CompileError::at(
                    self.source,
                    p.span.start,
                    format!("unconstrained impl parameter `{}`", p.name),
                ));
            }
            if let Some(scope) = self
                .trait_scopes
                .iter()
                .find(|s| s.span.contains(item.span.start))
            {
                if !scope.core && !scope.owned.contains(name) && !scope.owned.contains(outer) {
                    return Err(CompileError::at(
                        self.source,
                        item.span.start,
                        format!("orphan impl: this module owns neither `{name}` nor `{outer}`"),
                    ));
                }
            } else if name.starts_with("$std$")
                && (Ty::parse_name(outer).is_some()
                    || matches!(
                        outer.as_str(),
                        "Vec" | "Slice" | "Owned" | "Option" | "Result"
                    ))
            {
                return Err(CompileError::at(
                    self.source,
                    item.span.start,
                    "orphan impl for a core type",
                ));
            }
            for prior in &self.trait_impls {
                if prior.item.trait_name.as_ref().unwrap().0 == *name && overlaps(&prior.item, item)
                {
                    return Err(CompileError::at(
                        self.source,
                        item.span.start,
                        format!(
                            "overlapping implementations of `{name}` for {}",
                            parsed_brief(&item.ty)
                        ),
                    ));
                }
            }
            let mut methods = HashMap::new();
            for method in &item.items {
                if methods.contains_key(&method.name) {
                    return Err(CompileError::at(
                        self.source,
                        method.name_span.start,
                        format!("duplicate method `{}`", method.name),
                    ));
                }
                let Some(required) = tr.methods.iter().find(|m| m.name == method.name) else {
                    return Err(CompileError::at(
                        self.source,
                        method.name_span.start,
                        format!("`{name}` has no method `{}`", method.name),
                    ));
                };
                if method.receiver != required.receiver
                    || !method.type_params.is_empty()
                    || method.params.len() != required.params.len()
                {
                    return Err(CompileError::at(
                        self.source,
                        method.name_span.start,
                        format!(
                            "signature of `{name}::{}` does not match the trait",
                            method.name
                        ),
                    ));
                }
                self.uses.push(NameUse {
                    span: method.name_span,
                    def: required.name_span,
                    def_file: None,
                    ty: trait_method_signature(required),
                });
                let func = format!("{name}${outer}$impl{index}${}", method.name);
                let receiver = receiver_ty(&item.ty, method.receiver.unwrap(), method.name_span);
                let mut params = vec![Param {
                    name: "self".into(),
                    name_span: method.name_span,
                    ty: Some(receiver),
                    pat: Pat::Name {
                        name: "self".into(),
                        span: method.name_span,
                    },
                }];
                params.extend(method.params.clone());
                for param in &mut params {
                    if let Some(ty) = &mut param.ty {
                        qualify_impl_type(ty, &item.ty, name);
                    }
                }
                let mut ret = method.ret.clone().unwrap_or_else(unit);
                qualify_impl_type(&mut ret, &item.ty, name);
                self.impl_contexts.insert(func.clone(), name.clone());
                module.functions.push(Function {
                    name: func.clone(),
                    name_span: method.name_span,
                    type_params: item.type_params.clone(),
                    params,
                    ret: Some(ret),
                    body: method.body.clone(),
                    c_export: false,
                    exported: false,
                });
                methods.insert(method.name.clone(), func);
            }
            for required in &tr.methods {
                if !methods.contains_key(&required.name) {
                    return Err(CompileError::at(
                        self.source,
                        item.span.start,
                        format!("missing method `{name}::{}`", required.name),
                    ));
                }
            }
            self.trait_impls.push(std::rc::Rc::new(TraitImpl {
                item: item.clone(),
                methods,
            }));
        }
        Ok(())
    }

    pub(super) fn register_trait_signatures(&mut self) -> Result<(), CompileError> {
        for tr in self.traits.clone() {
            for method in tr.methods {
                for p in &method.params {
                    if let Some(ty) = &p.ty {
                        self.validate_ty(ty, &["Self".into()])?;
                    }
                }
                if let Some(ty) = &method.ret {
                    self.validate_ty(ty, &["Self".into()])?;
                }
                if method
                    .params
                    .iter()
                    .any(|p| p.ty.as_ref().is_some_and(depends_on_self))
                    || method.ret.as_ref().is_some_and(depends_on_self)
                {
                    continue;
                }
                let mut params = Vec::new();
                for p in &method.params {
                    let ty = p.ty.as_ref().ok_or_else(|| {
                        CompileError::at(
                            self.source,
                            p.name_span.start,
                            "trait parameters need explicit types",
                        )
                    })?;
                    self.validate_ty(ty, &[])?;
                    params.push(self.apply(ty, &HashMap::new())?);
                }
                let parsed_ret = method.ret.clone().unwrap_or_else(unit);
                self.validate_ty(&parsed_ret, &[])?;
                let ret = self.apply(&parsed_ret, &HashMap::new())?;
                self.trait_sigs
                    .insert((tr.name.clone(), method.name), FuncSig { params, ret });
            }
        }
        Ok(())
    }

    pub(super) fn validate_traits(&mut self) -> Result<(), CompileError> {
        self.validate_associated_contracts()?;
        let schemas: Vec<_> = self.schemas.iter().map(|s| s.type_params.clone()).collect();
        for params in schemas {
            self.validate_bounds(&params)?;
        }
        for func in self.ast_fns.clone() {
            self.validate_bounds(&func.type_params)?;
        }
        for implementation in self.trait_impls.clone() {
            let item = &implementation.item;
            let names: Vec<_> = item.type_params.iter().map(|p| p.name.clone()).collect();
            self.validate_ty(&item.ty, &names)?;
            let mut subst = HashMap::new();
            for p in &item.type_params {
                let ty = self.rigid(p)?;
                subst.insert(p.name.clone(), ty);
            }
            let tr = self
                .traits
                .iter()
                .find(|t| t.name == item.trait_name.as_ref().unwrap().0)
                .unwrap()
                .clone();
            let self_ty = self.apply(&item.ty, &subst)?;
            let saved_self = self.assoc_self.replace((tr.name.clone(), self_ty));
            self.validate_associated_values(&tr, item, self_ty, &subst)?;
            let mut contract_subst = HashMap::from([("Self".into(), self_ty)]);
            // Associated definitions may depend on this impl's rigid parameters.
            contract_subst.extend(subst.clone());
            for method in &item.items {
                let required = tr.methods.iter().find(|m| m.name == method.name).unwrap();
                for (have, want) in method.params.iter().zip(&required.params) {
                    let have = self.apply(have.ty.as_ref().unwrap(), &subst)?;
                    let want = self.apply(want.ty.as_ref().unwrap(), &contract_subst)?;
                    if have != want {
                        return Err(CompileError::at(
                            self.source,
                            method.name_span.start,
                            format!(
                                "signature of `{}::{}` does not match the trait",
                                tr.name, method.name
                            ),
                        ));
                    }
                }
                let have = self.apply(&method.ret.clone().unwrap_or_else(unit), &subst)?;
                let want =
                    self.apply(&required.ret.clone().unwrap_or_else(unit), &contract_subst)?;
                if have != want {
                    return Err(CompileError::at(
                        self.source,
                        method.name_span.start,
                        format!(
                            "return type of `{}::{}` does not match the trait",
                            tr.name, method.name
                        ),
                    ));
                }
            }
            self.assoc_self = saved_self;
        }
        Ok(())
    }
}

fn receiver_ty(ty: &ParsedTy, recv: Recv, span: Span) -> ParsedTy {
    match recv {
        Recv::Value => ty.clone(),
        _ => ParsedTy::Ref {
            span,
            mutable: recv == Recv::Mut,
            inner: Box::new(ty.clone()),
        },
    }
}
fn unit() -> ParsedTy {
    ParsedTy::Named {
        span: Span::empty(),
        name: "unit".into(),
        args: Vec::new(),
    }
}

fn collect_parameters(ty: &ParsedTy, names: &mut Vec<String>) {
    match ty {
        ParsedTy::Projection { base, .. } => collect_parameters(base, names),
        ParsedTy::Named { name, args, .. } => {
            names.push(name.clone());
            for arg in args {
                collect_parameters(arg, names);
            }
        }
        ParsedTy::Ref { inner, .. } | ParsedTy::Ptr { inner, .. } => {
            collect_parameters(inner, names)
        }
        ParsedTy::Tuple { fields, .. } => {
            for field in fields {
                collect_parameters(field, names);
            }
        }
        ParsedTy::Array { elem, .. } => collect_parameters(elem, names),
        ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
            for p in params {
                collect_parameters(p, names);
            }
            collect_parameters(ret, names);
        }
    }
}

fn label(name: &str) -> &str {
    name.rsplit('$').next().unwrap_or(name)
}
