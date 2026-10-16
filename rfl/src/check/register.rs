//! Register in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn register(&mut self, module: &mut Module) -> Result<(), CompileError> {
        self.declare_structs(&module.structs)?;
        for adt in &module.adts {
            if self.schemas.iter().any(|schema| schema.name == adt.name)
                || self.struct_names.contains_key(&adt.name)
                || adt.name == "Owned"
            {
                return Err(CompileError::at(
                    self.source,
                    adt.name_span.start,
                    format!("duplicate type `{}`", adt.name),
                ));
            }
            let mut params = Vec::new();
            for param in &adt.params {
                if params.contains(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("duplicate type parameter `{}`", param.name),
                    ));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("`{}` is not a valid type parameter", param.name),
                    ));
                }
                params.push(param.name.clone());
            }
            self.schemas.push(AdtSchema {
                name: adt.name.clone(),
                type_params: adt.params.clone(),
                params,
                ctors: Vec::new(),
            });
            if adt.name_span.end > adt.name_span.start {
                self.type_defs.insert(adt.name.clone(), adt.name_span);
            }
        }
        for (index, adt) in module.adts.iter().enumerate() {
            for ctor in &adt.ctors {
                if is_builtin(&ctor.name) {
                    return Err(CompileError::at(
                        self.source,
                        ctor.name_span.start,
                        format!("`{}` is a builtin", ctor.name),
                    ));
                }
                if self.ctor_schema.contains_key(&ctor.name) {
                    return Err(CompileError::at(
                        self.source,
                        ctor.name_span.start,
                        format!("duplicate constructor `{}`", ctor.name),
                    ));
                }
                for field in &ctor.fields {
                    if escaped_ref(field) {
                        return Err(CompileError::at(
                            self.source,
                            field.span().start,
                            "a reference cannot be stored in a type",
                        ));
                    }
                    self.validate_ty(field, &self.schemas[index].params.clone())?;
                }
                let tag = self.schemas[index].ctors.len() as u16;
                self.schemas[index].ctors.push(ctor.fields.clone());
                self.ctor_schema.insert(ctor.name.clone(), (index, tag));
                if ctor.name_span.end > ctor.name_span.start {
                    let ret = if adt.params.is_empty() {
                        adt.name.clone()
                    } else {
                        format!(
                            "{}<{}>",
                            adt.name,
                            adt.params
                                .iter()
                                .map(|param| param.name.as_str())
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    };
                    let ty = if ctor.fields.is_empty() {
                        ret
                    } else {
                        format!(
                            "fn({}) -> {ret}",
                            ctor.fields
                                .iter()
                                .map(parsed_brief)
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    };
                    self.facts.push(BindingFact {
                        name: ctor.name.clone(),
                        def: ctor.name_span,
                        ty,
                        scope: Span::new(0, self.source.len()),
                        visible_from: 0,
                        variant: true,
                    });
                }
            }
        }
        self.eval_consts(&module.consts)?;
        self.finish_structs(&module.structs)?;
        let plain: Vec<usize> = self
            .schemas
            .iter()
            .enumerate()
            .filter(|(_, schema)| schema.params.is_empty())
            .map(|(index, _)| index)
            .collect();
        for index in plain {
            self.monomorphize(index, &[])?;
        }
        for func in &mut module.functions {
            if func.ret.is_none()
                && func.body.tail.is_none()
                && (!func.type_params.is_empty() || !always_returns(&func.body))
            {
                func.ret = Some(ParsedTy::Named {
                    span: func.name_span,
                    name: "unit".into(),
                    args: Vec::new(),
                });
            }
        }
        self.infer_omitted(&mut module.functions)?;
        self.ast_fns = module.functions.clone();
        self.register_externs(&module.externs)?;
        for (index, func) in module.functions.iter().enumerate() {
            if is_builtin(&func.name) {
                return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("`{}` is a builtin", func.name),
                ));
            }
            if self.sigs.contains_key(&func.name)
                || self.fn_schemas.contains_key(&func.name)
                || self.ctor_schema.contains_key(&func.name)
            {
                return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("duplicate function `{}`", func.name),
                ));
            }
            let mut type_params = Vec::new();
            for param in &func.type_params {
                if type_params.contains(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("duplicate type parameter `{}`", param.name),
                    ));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("`{}` is not a valid type parameter", param.name),
                    ));
                }
                type_params.push(param.name.clone());
            }
            for param in &func.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                self.validate_ty(ty, &type_params)?;
            }
            if let Some(ret) = &func.ret {
                if escaped_ref(ret) {
                    return Err(CompileError::at(
                        self.source,
                        ret.span().start,
                        "a function cannot return a reference",
                    ));
                }
                self.validate_ty(ret, &type_params)?;
            }
            if func.c_export {
                if !type_params.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "an exported function cannot be generic",
                    ));
                }
                if func.name == "main" {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "cannot export `main`",
                    ));
                }
            }
            if !type_params.is_empty() {
                if func.name == "main" {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "main cannot be generic",
                    ));
                }
                if func.ret.is_none() {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        format!("generic function `{}` needs a return type", func.name),
                    ));
                }
                self.fn_schemas
                    .insert(func.name.clone(), FnSchema { index, type_params });
                self.note_fn_fact(func);
                continue;
            }
            let mut params = Vec::new();
            for param in &func.params {
                params.push(self.apply(param.ty.as_ref().unwrap(), &HashMap::new())?);
            }
            let ret = match &func.ret {
                Some(ty) => Some(self.apply(ty, &HashMap::new())?),
                None => None,
            };
            self.sigs.insert(
                func.name.clone(),
                Sig {
                    params,
                    ret,
                    c_abi: func.c_export,
                    variadic: false,
                },
            );
            self.note_fn_fact(func);
        }
        Ok(())
    }

    pub(super) fn note_fn_fact(&mut self, func: &Function) {
        if func.name_span.end <= func.name_span.start {
            return;
        }
        let written = self
            .source
            .get(func.name_span.start..func.name_span.end)
            .unwrap_or(func.name.as_str())
            .to_string();
        self.facts.push(BindingFact {
            name: written,
            def: func.name_span,
            ty: parsed_signature(func, false),
            scope: Span::new(0, self.source.len()),
            visible_from: 0,
            variant: false,
        });
    }
}
