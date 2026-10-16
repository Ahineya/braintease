//! Associated types normalize to existing types. Rigid projections exist only
//! in the declaration-checking clone and never reach runtime layouts or IR.
use super::*;
mod editor;

pub(super) fn contains_projection(ty: &ParsedTy) -> bool {
    match ty {
        ParsedTy::Projection { .. } => true,
        ParsedTy::Named { args, .. } => args.iter().any(contains_projection),
        ParsedTy::Ref { inner, .. } | ParsedTy::Ptr { inner, .. } => contains_projection(inner),
        ParsedTy::Array { elem, .. } => contains_projection(elem),
        ParsedTy::Tuple { fields, .. } => fields.iter().any(contains_projection),
        ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
            params.iter().any(contains_projection) || contains_projection(ret)
        }
    }
}

pub(super) fn depends_on_self(ty: &ParsedTy) -> bool {
    match ty {
        ParsedTy::Projection { .. } => true,
        ParsedTy::Named { name, args, .. } => name == "Self" || args.iter().any(depends_on_self),
        ParsedTy::Ref { inner, .. } | ParsedTy::Ptr { inner, .. } => depends_on_self(inner),
        ParsedTy::Array { elem, .. } => depends_on_self(elem),
        ParsedTy::Tuple { fields, .. } => fields.iter().any(depends_on_self),
        ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
            params.iter().any(depends_on_self) || depends_on_self(ret)
        }
    }
}

/// Headers of expanded impl functions must resolve before entering their body.
/// Qualify Self projections with this impl's contract, regardless of other traits.
pub(super) fn qualify_impl_type(ty: &mut ParsedTy, owner: &ParsedTy, contract: &str) {
    match ty {
        ParsedTy::Projection {
            base, trait_name, ..
        } => {
            if trait_name.is_none()
                && matches!(base.as_ref(), ParsedTy::Named { name, .. } if name == "Self")
            {
                *trait_name = Some((contract.into(), Span::empty()));
            }
            qualify_impl_type(base, owner, contract);
        }
        ParsedTy::Named { name, span, args } if name == "Self" && args.is_empty() => {
            let position = *span;
            *ty = owner.clone();
            if let ParsedTy::Named { span, .. } = ty {
                *span = position;
            }
        }
        ParsedTy::Named { args, .. } => {
            for arg in args {
                qualify_impl_type(arg, owner, contract);
            }
        }
        ParsedTy::Ref { inner, .. } | ParsedTy::Ptr { inner, .. } => {
            qualify_impl_type(inner, owner, contract)
        }
        ParsedTy::Array { elem, .. } => qualify_impl_type(elem, owner, contract),
        ParsedTy::Tuple { fields, .. } => {
            for field in fields {
                qualify_impl_type(field, owner, contract);
            }
        }
        ParsedTy::Func { params, ret, .. } | ParsedTy::CFn { params, ret, .. } => {
            for param in params {
                qualify_impl_type(param, owner, contract);
            }
            qualify_impl_type(ret, owner, contract);
        }
    }
}

impl Checker<'_> {
    /// Gather known receiver/value types before context-checking a projection
    /// argument. This only inspects types: borrow checking and expression checking
    /// still run once in source order. A projection never determines its base.
    pub(super) fn infer_projection_arguments(
        &mut self,
        func: &Function,
        args: &[Expr],
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
        loose_first: bool,
    ) -> Result<(), CompileError> {
        if !func
            .params
            .iter()
            .any(|p| p.ty.as_ref().is_some_and(contains_projection))
        {
            return Ok(());
        }
        for (index, (param, arg)) in func.params.iter().zip(args).enumerate() {
            let parsed = param.ty.as_ref().unwrap();
            if (index == 0 && loose_first)
                || contains_projection(parsed)
                || !mentions_unsolved(parsed, params, solved)
            {
                continue;
            }
            let hint = match &arg.kind {
                ExprKind::StructLit { name, .. } => {
                    self.struct_names.get(name).map(|id| Ty::Struct {
                        id: *id,
                        width: self.struct_width(*id),
                    })
                }
                ExprKind::Borrow { mutable, expr, .. } => {
                    self.peek(expr).map(|ty| self.intern_ref(*mutable, ty))
                }
                ExprKind::Int(_) => Some(Ty::I16),
                ExprKind::Float(_) => Some(Ty::F64),
                _ => self.peek(arg),
            };
            if let Some(ty) = hint {
                self.infer_field(parsed, ty, params, solved, span)?;
            }
        }
        Ok(())
    }

    pub(super) fn validate_associated_declarations(&self, tr: &Trait) -> Result<(), CompileError> {
        let mut names = HashSet::new();
        for item in &tr.associated_types {
            if !names.insert(&item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate associated type `{}`", item.name),
                ));
            }
            if item.value.is_some() {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    "associated type defaults are not supported",
                ));
            }
        }
        Ok(())
    }

    pub(super) fn validate_associated_definitions(
        &mut self,
        tr: &Trait,
        implementation: &Impl,
    ) -> Result<(), CompileError> {
        let mut names = HashSet::new();
        for item in &implementation.associated_types {
            if !names.insert(&item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate associated type `{}`", item.name),
                ));
            }
            let Some(required) = tr.associated_types.iter().find(|a| a.name == item.name) else {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("`{}` has no associated type `{}`", tr.name, item.name),
                ));
            };
            if item.value.is_none() {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    "associated type definition needs a type",
                ));
            }
            self.uses.push(NameUse {
                span: item.name_span,
                def: required.name_span,
                def_file: None,
                ty: format!(
                    "type {} = {}",
                    item.name,
                    parsed_brief(item.value.as_ref().unwrap())
                ),
            });
        }
        for required in &tr.associated_types {
            if !names.contains(&required.name) {
                return Err(CompileError::at(
                    self.source,
                    implementation.span.start,
                    format!("missing associated type `{}::{}`", tr.name, required.name),
                ));
            }
        }
        Ok(())
    }

    pub(super) fn validate_associated_contracts(&mut self) -> Result<(), CompileError> {
        for tr in self.traits.clone() {
            for item in &tr.associated_types {
                self.validate_bounds(&[TypeParam {
                    name: item.name.clone(),
                    span: item.name_span,
                    bounds: item.bounds.clone(),
                }])?;
                self.facts.push(BindingFact {
                    name: item.name.clone(),
                    def: item.name_span,
                    ty: format!("type {}::{}", tr.name, item.name),
                    scope: item.name_span,
                    visible_from: item.name_span.start,
                    variant: false,
                });
            }
            let self_ty = self.rigid(&TypeParam {
                name: "Self".into(),
                span: tr.name_span,
                bounds: vec![(tr.name.clone(), tr.name_span)],
            })?;
            let saved = self.assoc_self.replace((tr.name.clone(), self_ty));
            for method in &tr.methods {
                self.projected_method_signature(&tr.name, &method.name, self_ty)?;
            }
            self.assoc_self = saved;
        }
        Ok(())
    }

    pub(super) fn projected_method_signature(
        &mut self,
        name: &str,
        method: &str,
        ty: Ty,
    ) -> Result<FuncSig, CompileError> {
        let required = self
            .traits
            .iter()
            .find(|tr| tr.name == name)
            .and_then(|tr| tr.methods.iter().find(|m| m.name == method))
            .unwrap()
            .clone();
        let saved = self.assoc_self.replace((name.into(), ty));
        let subst = HashMap::from([("Self".into(), ty)]);
        let result = (|| {
            let mut params = Vec::new();
            for param in &required.params {
                params.push(self.apply(
                    param.ty.as_ref().ok_or_else(|| {
                        CompileError::at(
                            self.source,
                            param.name_span.start,
                            "trait parameters need explicit types",
                        )
                    })?,
                    &subst,
                )?);
            }
            let ret = required
                .ret
                .as_ref()
                .map(|ret| self.apply(ret, &subst))
                .transpose()?
                .unwrap_or(Ty::Unit);
            if ret.is_ref() {
                return Err(CompileError::at(
                    self.source,
                    required.name_span.start,
                    "a function cannot return a reference",
                ));
            }
            Ok(FuncSig { params, ret })
        })();
        self.assoc_self = saved;
        result
    }

    pub(super) fn apply_projection(
        &mut self,
        span: Span,
        base: Ty,
        explicit: Option<&str>,
        member: &str,
        name_span: Span,
        qualifier_span: Option<Span>,
    ) -> Result<Ty, CompileError> {
        let mut candidates = Vec::new();
        if let Some(name) = explicit {
            let name = self.trait_name(name).ok_or_else(|| {
                CompileError::at(self.source, span.start, format!("unknown trait `{name}`"))
            })?;
            if !self.satisfies(&name, base)? {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    format!("{} does not implement `{name}`", self.show(base)),
                ));
            }
            candidates.push(name);
        } else {
            let visible = self
                .trait_scopes
                .iter()
                .find(|s| s.span.contains(span.start));
            for tr in &self.traits {
                if visible.is_some_and(|s| !s.visible.contains(&tr.name)) {
                    continue;
                }
                if tr.associated_types.iter().any(|a| a.name == member)
                    && self.satisfies(&tr.name, base)?
                {
                    candidates.push(tr.name.clone());
                }
            }
        }
        if candidates.len() > 1 {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                format!(
                    "ambiguous associated type `{member}` for {}; use `<Type as Trait>::{member}`",
                    self.show(base)
                ),
            ));
        }
        let name = candidates.pop().ok_or_else(|| {
            CompileError::at(
                self.source,
                name_span.start,
                format!(
                    "{} has no associated type `{member}` in its bounds or visible traits",
                    self.show(base)
                ),
            )
        })?;
        let tr = self
            .traits
            .iter()
            .find(|tr| tr.name == name)
            .unwrap()
            .clone();
        let required = tr
            .associated_types
            .iter()
            .find(|a| a.name == member)
            .ok_or_else(|| {
                CompileError::at(
                    self.source,
                    name_span.start,
                    format!("`{name}` has no associated type `{member}`"),
                )
            })?
            .clone();
        let key = (base, name.clone(), member.into());
        let ty = if self.opaque.contains_key(&base) {
            if let Some(ty) = self.projection_types.get(&key) {
                *ty
            } else {
                let ty = self.rigid(&TypeParam {
                    name: format!(
                        "<{} as {}>::{member}",
                        self.show(base),
                        name.rsplit('$').next().unwrap_or(&name)
                    ),
                    span: required.name_span,
                    bounds: required.bounds.clone(),
                })?;
                self.projection_types.insert(key, ty);
                ty
            }
        } else {
            if self.projection_stack.contains(&key) || self.projection_stack.len() >= 64 {
                return Err(CompileError::at(
                    self.source,
                    name_span.start,
                    format!(
                        "associated type resolution cycle at {}::{member}",
                        self.show(base)
                    ),
                ));
            }
            let implementation = self
                .trait_impls
                .iter()
                .find(|i| {
                    i.item.trait_name.as_ref().unwrap().0 == name
                        && self.impl_args(&i.item, base).is_some()
                })
                .unwrap()
                .clone();
            let args = self.impl_args(&implementation.item, base).unwrap();
            let mut subst: HashMap<_, _> = implementation
                .item
                .type_params
                .iter()
                .zip(args)
                .map(|(p, ty)| (p.name.clone(), ty))
                .collect();
            subst.insert("Self".into(), base);
            let value = implementation
                .item
                .associated_types
                .iter()
                .find(|a| a.name == member)
                .unwrap()
                .value
                .as_ref()
                .unwrap();
            self.projection_stack.push(key);
            let saved_context = self.assoc_self.replace((name.clone(), base));
            let saved_subst = std::mem::take(&mut self.subst);
            let result = self.apply(value, &subst);
            self.subst = saved_subst;
            self.assoc_self = saved_context;
            self.projection_stack.pop();
            result?
        };
        if self.editor {
            self.uses.push(NameUse {
                span: name_span,
                def: required.name_span,
                def_file: None,
                ty: format!("type {member} = {}", self.show(ty)),
            });
            if let Some(span) = qualifier_span.filter(|s| s.end > s.start) {
                self.uses.push(NameUse {
                    span,
                    def: tr.name_span,
                    def_file: None,
                    ty: format!("trait {}", name.rsplit('$').next().unwrap_or(&name)),
                });
            }
        }
        Ok(ty)
    }

    pub(super) fn validate_associated_values(
        &mut self,
        tr: &Trait,
        implementation: &Impl,
        self_ty: Ty,
        subst: &HashMap<String, Ty>,
    ) -> Result<(), CompileError> {
        let params: Vec<_> = implementation
            .type_params
            .iter()
            .map(|p| p.name.clone())
            .collect();
        for item in &implementation.associated_types {
            let value = item.value.as_ref().unwrap();
            self.validate_ty(value, &params)?;
            let ty = self.apply_projection(
                value.span(),
                self_ty,
                Some(&tr.name),
                &item.name,
                item.name_span,
                None,
            )?;
            if self.associated_borrows(ty) {
                return Err(CompileError::at(
                    self.source,
                    value.span().start,
                    "an associated type cannot contain a borrowed reference",
                ));
            }
            let required = tr
                .associated_types
                .iter()
                .find(|a| a.name == item.name)
                .unwrap();
            for (bound, _) in &required.bounds {
                let bound = self.trait_name(bound).unwrap();
                if !self.satisfies(&bound, ty)? {
                    return Err(CompileError::at(
                        self.source,
                        item.name_span.start,
                        format!(
                            "associated type `{}`: {} does not implement `{bound}`",
                            item.name,
                            self.show(ty)
                        ),
                    ));
                }
            }
            // Also records uses in aliases that contain generic parameters.
            self.apply(value, subst)?;
        }
        Ok(())
    }

    fn associated_borrows(&self, ty: Ty) -> bool {
        match ty {
            Ty::Ref(_) => true,
            Ty::Ptr(id) => self.associated_borrows(self.ptrs[id as usize].inner),
            Ty::Tuple { id, .. } => self.tuples[id as usize]
                .iter()
                .any(|ty| self.associated_borrows(*ty)),
            Ty::Array { id, .. } => self.associated_borrows(self.arrays[id as usize].elem),
            Ty::Func(id) => self.associated_borrows(self.funcs[id as usize].ret),
            Ty::CFn(id) => self.associated_borrows(self.cfns[id as usize].ret),
            _ => false,
        }
    }
}
