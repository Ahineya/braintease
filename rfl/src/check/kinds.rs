//! First-order constructor kinds. Constructor identities are compile-time metadata;
//! only fully applied value types may enter function signatures or field layouts.
use super::*;
mod partials;

pub(super) fn is_slot(ty: &ParsedTy) -> bool {
    matches!(ty, ParsedTy::Named { name, args, .. } if name == "_" && args.is_empty())
}

#[derive(Clone)]
pub(super) struct Constructor {
    pub name: String,
    pub arity: usize,
    target: Target,
}

#[derive(Clone, PartialEq, Eq)]
enum Target {
    Vec,
    Slice,
    Owned,
    Adt(usize),
    Rigid,
    Partial { base: Ty, slots: Vec<Option<Ty>> },
}

impl Checker<'_> {
    fn intern_constructor(&mut self, name: String, arity: usize, target: Target) -> Ty {
        let ty = Ty::Constructor(self.constructors.len() as u32);
        self.constructors.push(Constructor {
            name,
            arity,
            target,
        });
        ty
    }

    pub(super) fn register_constructors(&mut self) {
        for (name, target) in [
            ("Vec", Target::Vec),
            ("Slice", Target::Slice),
            ("Owned", Target::Owned),
        ] {
            let ty = self.intern_constructor(name.into(), 1, target);
            self.constructor_names.insert(name.into(), ty);
        }
        for index in 0..self.schemas.len() {
            let schema = &self.schemas[index];
            // This milestone accepts constructors whose arguments are value types.
            // Constructors taking other constructors need higher-order kinds.
            if !schema.type_params.is_empty() && schema.type_params.iter().all(|p| p.arity == 0) {
                let name = schema.name.clone();
                let ty =
                    self.intern_constructor(name.clone(), schema.params.len(), Target::Adt(index));
                self.constructor_names.insert(name, ty);
            }
        }
    }

    pub(super) fn rigid_constructor(&mut self, param: &TypeParam) -> Ty {
        let ty = self.intern_constructor(param.name.clone(), param.arity, Target::Rigid);
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
        ty
    }

    pub(super) fn check_kind(&self, ty: Ty, arity: usize, span: usize) -> Result<(), CompileError> {
        let found = match ty {
            Ty::Constructor(id) => self.constructors[id as usize].arity,
            _ => 0,
        };
        if found == arity {
            return Ok(());
        }
        Err(CompileError::at(
            self.source,
            span,
            if arity == 0 {
                format!(
                    "type constructor `{}` needs arguments in a value type",
                    self.show(ty)
                )
            } else {
                format!("expected a constructor taking {arity} value type arguments, found {} (arity {found})", self.show(ty))
            },
        ))
    }

    pub(super) fn validate_argument(
        &self,
        ty: &ParsedTy,
        arity: usize,
        params: &[TypeParam],
    ) -> Result<(), CompileError> {
        if arity == 0 {
            return self.validate_ty(ty, params);
        }
        if let ParsedTy::Named { name, args, .. } = ty {
            let found = if let Some(param) = params.iter().find(|p| p.name == *name) {
                param.arity
            } else if let Some(Ty::Constructor(id)) = self.constructor_names.get(name) {
                self.constructors[*id as usize].arity
            } else {
                0
            };
            if args.is_empty() && found == arity {
                return Ok(());
            }
            if !args.is_empty() && found > 0 {
                if args.len() != found {
                    return Err(self.arity_error(ty.span().start, name, found, args.len()));
                }
                let slots = args.iter().filter(|arg| is_slot(arg)).count();
                if slots == arity {
                    for arg in args.iter().filter(|arg| !is_slot(arg)) {
                        self.validate_ty(arg, params)?;
                    }
                    return Ok(());
                }
            }
        }
        Err(CompileError::at(
            self.source,
            ty.span().start,
            format!("expected a constructor with {arity} open slots (bare or partially applied)"),
        ))
    }

    pub(super) fn apply_argument(
        &mut self,
        ty: &ParsedTy,
        arity: usize,
        subst: &HashMap<String, Ty>,
    ) -> Result<Ty, CompileError> {
        if arity == 0 {
            return self.apply(ty, subst);
        }
        if let ParsedTy::Named { name, args, span } = ty {
            if let Some(found) = subst
                .get(name)
                .or_else(|| self.subst.get(name))
                .or_else(|| self.constructor_names.get(name))
                .copied()
                .or_else(|| {
                    (name == "Self")
                        .then(|| self.assoc_self.as_ref().map(|(_, ty)| *ty))
                        .flatten()
                })
            {
                let found = if args.is_empty() {
                    found
                } else {
                    let Ty::Constructor(id) = found else {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            "a value type is not a type constructor",
                        ));
                    };
                    let expected = self.constructors[id as usize].arity;
                    if args.len() != expected {
                        return Err(self.arity_error(span.start, name, expected, args.len()));
                    }
                    let mut slots = Vec::new();
                    for arg in args {
                        slots.push(if is_slot(arg) {
                            None
                        } else {
                            Some(self.apply(arg, subst)?)
                        });
                    }
                    self.partial_constructor(found, slots, span.start)?
                };
                self.check_kind(found, arity, span.start)?;
                self.note_type(*span, name);
                return Ok(found);
            }
        }
        Err(CompileError::at(
            self.source,
            ty.span().start,
            format!("expected a constructor with {arity} open slots (bare or partially applied)"),
        ))
    }

    pub(super) fn apply_constructor(
        &mut self,
        constructor: Ty,
        args: Vec<Ty>,
        span: usize,
    ) -> Result<Ty, CompileError> {
        let Ty::Constructor(id) = constructor else {
            return Err(CompileError::at(
                self.source,
                span,
                "a value type is not a type constructor",
            ));
        };
        let info = self.constructors[id as usize].clone();
        if args.len() != info.arity {
            return Err(self.arity_error(span, &info.name, info.arity, args.len()));
        }
        for arg in &args {
            self.check_kind(*arg, 0, span)?;
        }
        Ok(match info.target {
            Target::Partial { base, slots } => {
                let mut arguments = args.into_iter();
                let filled = slots
                    .into_iter()
                    .map(|slot| slot.unwrap_or_else(|| arguments.next().unwrap()))
                    .collect();
                self.apply_constructor(base, filled, span)?
            }
            Target::Vec | Target::Slice | Target::Owned => {
                self.require_vec_elem(span, args[0])?;
                match info.target {
                    Target::Vec => self.intern_vec(args[0]),
                    Target::Slice => self.intern_slice(args[0]),
                    _ => self.intern_owned(args[0]),
                }
            }
            Target::Adt(schema) => Ty::Adt(self.monomorphize(schema, &args)?),
            Target::Rigid => {
                let key = (constructor, args.clone());
                if let Some(ty) = self.applications.get(&key) {
                    return Ok(*ty);
                }
                let ty = self.rigid(&TypeParam {
                    name: applied_name(&info.name, &args, self),
                    arity: 0,
                    bounds: Vec::new(),
                    span: Span::new(span, span),
                })?;
                self.applied_constructors.insert(ty, key.clone());
                self.applications.insert(key, ty);
                ty
            }
        })
    }

    pub(super) fn decompose_constructor(&self, ty: Ty) -> Option<(Ty, Vec<Ty>)> {
        if let Some(found) = self.applied_constructors.get(&ty) {
            return Some(found.clone());
        }
        let (name, args) = match ty {
            Ty::Vec(id) => ("Vec", vec![self.vec_elems[id as usize]]),
            Ty::Slice(id) => ("Slice", vec![self.slice_elems[id as usize]]),
            Ty::Owned(id) => ("Owned", vec![self.owner_elems[id as usize]]),
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                (self.schemas[adt.schema].name.as_str(), adt.args.clone())
            }
            _ => return None,
        };
        Some((*self.constructor_names.get(name)?, args))
    }
}
