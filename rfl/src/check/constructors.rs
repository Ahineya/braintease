//! Constructors in the RFL type checker.

use super::*;
mod arguments;

impl<'a> Checker<'a> {
    pub(super) fn nullary(
        &self,
        span: usize,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        if !self.schemas[schema].ctors[tag as usize].is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments",
                    self.schemas[schema].ctors[tag as usize].len()
                ),
            ));
        }
        let id = if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema != schema {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{name}` constructs {}",
                        self.show(Ty::Adt(id)),
                        self.schemas[schema].name
                    ),
                ));
            }
            id
        } else if self.schemas[schema].params.is_empty() {
            let id = self.mono[&(schema, Vec::new())];
            if let Some(want) = expected {
                if want != Ty::Adt(id) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `{name}` has type {}",
                            self.show(want),
                            self.show(Ty::Adt(id))
                        ),
                    ));
                }
            }
            id
        } else if let Some(want) = expected {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, `{name}` constructs {}",
                    self.show(want),
                    self.schemas[schema].name
                ),
            ));
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{name}`"),
            ));
        };
        Ok(TExpr {
            ty: Ty::Adt(id),
            kind: TExprKind::Ctor {
                tag,
                args: Vec::new(),
            },
        })
    }

    pub(super) fn check_ctor(
        &mut self,
        span: usize,
        name: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let (schema, tag) = self.ctor_schema[name];
        let params = self.schemas[schema].params.clone();
        let fields = self.schemas[schema].ctors[tag as usize].clone();
        if !type_args.is_empty() && params.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` does not take type arguments"),
            ));
        }
        if args.len() != fields.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments, found {}",
                    fields.len(),
                    args.len()
                ),
            ));
        }
        let mut solved = vec![None; params.len()];
        if !type_args.is_empty() {
            if type_args.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{name}` expects {} type arguments, found {}",
                        params.len(),
                        type_args.len()
                    ),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                if kinds::is_slot(ty) {
                    continue;
                }
                solved[index] = Some(self.apply_argument(
                    ty,
                    self.schemas[schema].type_params[index].arity,
                    &HashMap::new(),
                )?);
            }
        }
        if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema == schema {
                for (index, arg) in self.adts[id as usize].args.iter().enumerate() {
                    if solved[index].is_some_and(|explicit| explicit != *arg) {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "expected {}, found {}",
                                self.show(*arg),
                                self.show(solved[index].unwrap())
                            ),
                        ));
                    }
                    solved[index] = Some(*arg);
                }
            }
        }
        let mut checked = Vec::new();
        for (arg, field) in args.iter().zip(&fields) {
            if let Some(ty) = self.concrete_field(field, &params, &solved)? {
                let typed = self.check_expr(arg, Some(ty))?;
                if typed.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                checked.push(typed);
            } else {
                let typed = self.check_inferred_argument(arg, field, &params, &solved)?;
                self.infer_field(field, typed.ty, &params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{name}`"),
            ));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let id = self.monomorphize(schema, &concrete)?;
        let ty = Ty::Adt(id);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{name}` has type {}",
                        self.show(want),
                        self.show(ty)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Ctor { tag, args: checked },
        })
    }

    pub(super) fn concrete_field(
        &mut self,
        field: &ParsedTy,
        params: &[String],
        solved: &[Option<Ty>],
    ) -> Result<Option<Ty>, CompileError> {
        if mentions_unsolved(field, params, solved) {
            return Ok(None);
        }
        let mut subst = HashMap::new();
        for (index, param) in params.iter().enumerate() {
            if let Some(ty) = solved[index] {
                subst.insert(param.clone(), ty);
            }
        }
        Ok(Some(self.apply(field, &subst)?))
    }

    pub(super) fn infer_field(
        &self,
        parsed: &ParsedTy,
        actual: Ty,
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
    ) -> Result<(), CompileError> {
        match parsed {
            // Projections are not injective: never infer T from T::Output.
            // Once T is known, concrete_field and the final call check normalize it.
            ParsedTy::Projection { .. } => Ok(()),
            ParsedTy::CFn { .. } => Err(CompileError::at(
                self.source,
                span,
                "cannot infer a type through an `extern \"C\"` function",
            )),
            ParsedTy::Func {
                once,
                params: fn_params,
                ret,
                ..
            } => {
                let Ty::Func(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected a function, found {}", self.show(actual)),
                    ));
                };
                let sig = self.funcs[id as usize].clone();
                if *once != sig.once {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "consuming and reusable functions have different types",
                    ));
                }
                if fn_params.len() != sig.params.len() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {} parameters, found {}",
                            fn_params.len(),
                            sig.params.len()
                        ),
                    ));
                }
                for (param, got) in fn_params.iter().zip(sig.params) {
                    self.infer_field(param, got, params, solved, span)?;
                }
                self.infer_field(ret, sig.ret, params, solved, span)
            }
            ParsedTy::Ptr {
                span,
                mutable,
                inner,
            } => {
                let Ty::Ptr(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a pointer, found {}", self.show(actual)),
                    ));
                };
                if self.ptrs[id as usize].mutable != *mutable {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a pointer, found {}", self.show(actual)),
                    ));
                }
                let inner_ty = self.ptrs[id as usize].inner;
                self.infer_field(inner, inner_ty, params, solved, span.start)
            }
            ParsedTy::Ref {
                span,
                mutable,
                inner,
            } => {
                let Ty::Ref(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a reference, found {}", self.show(actual)),
                    ));
                };
                let actual_mut = self.refs[id as usize].mutable;
                let inner_ty = self.refs[id as usize].inner;
                if actual_mut != *mutable && !(self.loose_receiver && actual_mut && !*mutable) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "expected a {}, found {}",
                            if *mutable {
                                "mutable reference"
                            } else {
                                "shared reference"
                            },
                            self.show(actual)
                        ),
                    ));
                }
                self.infer_field(inner, inner_ty, params, solved, span.start)
            }
            ParsedTy::Tuple { span, fields } => {
                let Ty::Tuple { id, .. } = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a tuple, found {}", self.show(actual)),
                    ));
                };
                let got = self.tuples[id as usize].clone();
                if fields.len() != got.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {} fields, found {}", got.len(), fields.len()),
                    ));
                }
                for (field, ty) in fields.iter().zip(got) {
                    self.infer_field(field, ty, params, solved, span.start)?;
                }
                Ok(())
            }
            ParsedTy::Array { span, elem, len } => {
                let Ty::Array { id, len: got, .. } = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected an array, found {}", self.show(actual)),
                    ));
                };
                let (_, value) = self.eval_const(len, Some(Ty::Usize))?;
                if value != got as i64 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected length {value}, found {got}"),
                    ));
                }
                self.infer_field(
                    elem,
                    self.arrays[id as usize].elem,
                    params,
                    solved,
                    span.start,
                )
            }
            ParsedTy::Named {
                span: name_span,
                name,
                args,
            } => {
                if let Some(index) = params.iter().position(|param| param == name) {
                    if !args.is_empty() {
                        if matches!(actual, Ty::Constructor(_)) {
                            let head = solved[index].ok_or_else(|| CompileError::at(self.source, span,
                                "cannot infer the head of a partial constructor pattern; supply the constructor explicitly"))?;
                            return self
                                .infer_partial_pattern(head, args, actual, params, solved, span);
                        }
                        let (constructor, concrete) = if let Some(constructor) = solved[index] {
                            let concrete = self
                                .project_constructor(constructor, actual)
                                .ok_or_else(|| {
                                    CompileError::at(
                                        self.source,
                                        span,
                                        format!(
                                            "expected application of {}, found {}",
                                            self.show(constructor),
                                            self.show(actual)
                                        ),
                                    )
                                })?;
                            (constructor, concrete)
                        } else {
                            self.decompose_constructor(actual).ok_or_else(|| {
                                CompileError::at(
                                    self.source,
                                    span,
                                    format!(
                                        "expected application of constructor `{name}`, found {}",
                                        self.show(actual)
                                    ),
                                )
                            })?
                        };
                        if args.len() != concrete.len() {
                            if solved[index].is_none() {
                                return Err(CompileError::at(self.source, span,
                                    "cannot infer a partial constructor; supply explicit slots such as `Result<_, String>`"));
                            }
                            return Err(self.arity_error(
                                name_span.start,
                                name,
                                args.len(),
                                concrete.len(),
                            ));
                        }
                        match solved[index] {
                            None => solved[index] = Some(constructor),
                            Some(existing) if existing == constructor => {}
                            Some(existing) => {
                                return Err(CompileError::at(
                                    self.source,
                                    span,
                                    format!(
                                        "expected constructor {}, found {}",
                                        self.show(existing),
                                        self.show(constructor)
                                    ),
                                ))
                            }
                        }
                        for (arg, got) in args.iter().zip(concrete) {
                            self.infer_field(arg, got, params, solved, span)?;
                        }
                        return Ok(());
                    }
                    match solved[index] {
                        None => solved[index] = Some(actual),
                        Some(existing) if existing == actual => {}
                        Some(existing) => {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!(
                                    "expected {}, found {}",
                                    self.show(existing),
                                    self.show(actual)
                                ),
                            ));
                        }
                    }
                    return Ok(());
                }
                if matches!(actual, Ty::Constructor(_)) {
                    return self.infer_constructor_pattern(parsed, actual, params, solved, span);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if prim != actual {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {prim}, found {}", self.show(actual)),
                        ));
                    }
                    return Ok(());
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    let Ty::Struct { id: got, .. } = actual else {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {name}, found {}", self.show(actual)),
                        ));
                    };
                    if got != id {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {name}, found {}", self.show(actual)),
                        ));
                    }
                    return Ok(());
                }
                if name == "Vec" || name == "Slice" || name == "Owned" {
                    if args.len() != 1 {
                        return Err(self.arity_error(name_span.start, name, 1, args.len()));
                    }
                    let elem = if name == "Owned" {
                        let Ty::Owned(id) = actual else {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected Owned, found {}", self.show(actual)),
                            ));
                        };
                        self.owner_elems[id as usize]
                    } else if name == "Vec" {
                        let Ty::Vec(id) = actual else {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected Vec, found {}", self.show(actual)),
                            ));
                        };
                        self.vec_elems[id as usize]
                    } else {
                        let Ty::Slice(id) = actual else {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected Slice, found {}", self.show(actual)),
                            ));
                        };
                        self.slice_elems[id as usize]
                    };
                    return self.infer_field(&args[0], elem, params, solved, span);
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                let Ty::Adt(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {name}, found {}", self.show(actual)),
                    ));
                };
                if self.adts[id as usize].schema != schema {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {name}, found {}", self.show(actual)),
                    ));
                }
                let concrete = self.adts[id as usize].args.clone();
                if args.len() != concrete.len() {
                    return Err(self.arity_error(
                        name_span.start,
                        name,
                        concrete.len(),
                        args.len(),
                    ));
                }
                for (arg, got) in args.iter().zip(concrete) {
                    self.infer_field(arg, got, params, solved, span)?;
                }
                Ok(())
            }
        }
    }
}
