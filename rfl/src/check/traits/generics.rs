//! Rigid declaration checking and recursive bound satisfaction.
use super::*;
impl Checker<'_> {
    pub(in crate::check) fn validate_bounds(
        &mut self,
        params: &[TypeParam],
    ) -> Result<(), CompileError> {
        for p in params {
            let mut seen = HashSet::new();
            for (name, span) in &p.bounds {
                if self.trait_name(name).is_none() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown trait `{name}`"),
                    ));
                }
                if !seen.insert(name) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("duplicate bound `{name}`"),
                    ));
                }
                let canonical = self.trait_name(name).unwrap();
                let tr = self.traits.iter().find(|t| t.name == canonical).unwrap();
                self.uses.push(NameUse {
                    span: *span,
                    def: tr.name_span,
                    def_file: None,
                    ty: format!("trait {}", label(&canonical)),
                });
            }
        }
        Ok(())
    }

    pub(in crate::check) fn rigid(&mut self, param: &TypeParam) -> Result<Ty, CompileError> {
        self.validate_bounds(std::slice::from_ref(param))?;
        let id = self.structs.len() as u32;
        let name = format!("$param${id}${}", param.name);
        self.structs.push(StructInfo {
            name: name.clone(),
            fields: Vec::new(),
            width: 1,
            repr_c: false,
        });
        self.struct_names.insert(name, id);
        let ty = Ty::Struct { id, width: 1 };
        self.opaque.insert(
            ty,
            (
                param.name.clone(),
                param
                    .bounds
                    .iter()
                    .map(|(name, _)| self.trait_name(name).unwrap())
                    .collect(),
            ),
        );
        Ok(ty)
    }

    pub(in crate::check) fn check_generic_declarations(&mut self) -> Result<(), CompileError> {
        self.generic_check = true;
        let mark = self.typed.len();
        let declarations: Vec<_> = self.fn_schemas.keys().cloned().collect();
        // Source order keeps diagnostics deterministic, regardless of hash seeds.
        let mut declarations = declarations;
        declarations.sort_by_key(|name| self.fn_schemas[name].index);
        for name in declarations {
            self.check_declaration(&name)?;
        }
        self.check_resource_declarations()?;
        self.typed.truncate(mark);
        self.generic_check = false;
        // Declaration-only instantiations must never suppress code generation.
        self.fn_mono.clear();
        Ok(())
    }

    pub(in crate::check) fn check_declaration(&mut self, name: &str) -> Result<(), CompileError> {
        if self.declarations_checked.contains(name) || self.declarations_active.contains(name) {
            return Ok(());
        }
        self.declarations_active.insert(name.into());
        let params = self.ast_fns[self.fn_schemas[name].index]
            .type_params
            .clone();
        let saved_skip = self.sig_skip_self;
        let saved_span = self.current_span;
        let mut args = Vec::new();
        for param in &params {
            args.push(self.rigid(param)?);
        }
        self.instantiate(name, &args)?;
        self.sig_skip_self = saved_skip;
        self.current_span = saved_span;
        self.declarations_active.remove(name);
        self.declarations_checked.insert(name.into());
        Ok(())
    }

    pub(in crate::check) fn check_bounds(
        &mut self,
        params: &[TypeParam],
        args: &[Ty],
        span: usize,
    ) -> Result<(), CompileError> {
        for (p, ty) in params.iter().zip(args) {
            for (name, _) in &p.bounds {
                let canonical = self.trait_name(name).ok_or_else(|| {
                    CompileError::at(self.source, p.span.start, format!("unknown trait `{name}`"))
                })?;
                if !self.satisfies(&canonical, *ty)? {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "{} does not implement `{}` (required by `{}`)",
                            self.show(*ty),
                            label(name),
                            p.name
                        ),
                    ));
                }
            }
        }
        Ok(())
    }
}
