//! Calls in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let skip_self = std::mem::replace(&mut self.sig_skip_self, false);
        if is_builtin(callee) {
            if !type_args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` does not take type arguments"),
                ));
            }
            return self.check_builtin(span, callee, args, expected);
        }
        if let Some(id) = self.resolve(callee) {
            return self.check_apply(span, callee, id, type_args, args, expected);
        }
        if self.ctor_schema.contains_key(callee) {
            return self.check_ctor(span, callee, type_args, args, expected);
        }
        if self.fn_schemas.contains_key(callee) {
            self.sig_skip_self = skip_self;
            return self.check_generic_call(span, callee, type_args, args, expected);
        }
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let Some(sig) = self.sigs.get(callee) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown function `{callee}`"),
            ));
        };
        let c_abi = sig.c_abi;
        let variadic = sig.variadic;
        let ret = if c_abi {
            sig.ret.unwrap_or(Ty::Unit)
        } else {
            let Some(ret) = sig.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{callee}` needs a return type before it can be called"),
                ));
            };
            ret
        };
        let params = sig.params.clone();
        let fixed = params.len();
        self.noted_fn(&params, ret, skip_self);
        if variadic {
            if args.len() < fixed {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{callee}` expects at least {fixed} arguments, found {}",
                        args.len()
                    ),
                ));
            }
        } else if args.len() != fixed {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {fixed} arguments, found {}", args.len()),
            ));
        }
        let loose_first = std::mem::replace(&mut self.loose_receiver, false);
        let mut checked = Vec::new();
        for (index, (arg, ty)) in args.iter().take(fixed).zip(params).enumerate() {
            let arg = if loose_first && index == 0 {
                let typed = self.check_expr(arg, None)?;
                if !self.ref_compatible(typed.ty, ty) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                typed
            } else {
                let mut typed = self.check_expr(arg, Some(ty))?;
                if c_abi {
                    typed = self.coerce_raw(span, typed, ty)?;
                }
                if typed.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                typed
            };
            checked.push(arg);
        }
        if variadic {
            for arg in args.iter().skip(fixed) {
                let typed = self.check_expr(arg, None)?;
                checked.push(self.promote_vararg(span, typed)?);
            }
        }
        let kind = if c_abi {
            TExprKind::Extern {
                callee: callee.to_string(),
                args: checked,
                fixed,
            }
        } else {
            TExprKind::Call {
                callee: callee.to_string(),
                args: checked,
            }
        };
        Ok(TExpr { ty: ret, kind })
    }

    pub(super) fn check_c_apply(
        &mut self,
        span: usize,
        callee: &str,
        binding: u32,
        ty: Ty,
        id: u32,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let sig = self.cfns[id as usize].clone();
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    sig.params.len(),
                    args.len()
                ),
            ));
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&sig.params) {
            let typed = self.check_expr(arg, Some(*param))?;
            if typed.ty != *param {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*param),
                        self.show(typed.ty)
                    ),
                ));
            }
            checked.push(typed);
        }
        if let Some(want) = expected {
            if want != sig.ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(sig.ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: sig.ret,
            kind: TExprKind::CApply {
                callee: Box::new(TExpr {
                    ty,
                    kind: TExprKind::Name(binding),
                }),
                args: checked,
            },
        })
    }

    pub(super) fn check_apply(
        &mut self,
        span: usize,
        callee: &str,
        binding: u32,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ty = self.bindings[binding as usize].ty;
        if let Ty::CFn(id) = ty {
            return self.check_c_apply(span, callee, binding, ty, id, type_args, args, expected);
        }
        let Ty::Func(id) = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` is not a function"),
            ));
        };
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let sig = self.funcs[id as usize].clone();
        self.noted_fn(&sig.params, sig.ret, false);
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    sig.params.len(),
                    args.len()
                ),
            ));
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&sig.params) {
            let typed = self.check_expr(arg, Some(*param))?;
            if typed.ty != *param {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*param),
                        self.show(typed.ty)
                    ),
                ));
            }
            checked.push(typed);
        }
        if let Some(want) = expected {
            if want != sig.ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(sig.ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: sig.ret,
            kind: TExprKind::Apply {
                callee: Box::new(TExpr {
                    ty,
                    kind: TExprKind::Name(binding),
                }),
                args: checked,
                params: sig.params,
            },
        })
    }

    pub(super) fn check_generic_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let skip_self = std::mem::replace(&mut self.sig_skip_self, false);
        let type_params = self.fn_schemas[callee].type_params.clone();
        let func = self.ast_fns[self.fn_schemas[callee].index].clone();
        self.note_sig(parsed_signature(&func, skip_self));
        if args.len() != func.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    func.params.len(),
                    args.len()
                ),
            ));
        }
        let loose_first = std::mem::replace(&mut self.loose_receiver, false);
        let mut solved = vec![None; type_params.len()];
        if !type_args.is_empty() {
            if type_args.len() != type_params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{callee}` expects {} type arguments, found {}",
                        type_params.len(),
                        type_args.len()
                    ),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                solved[index] = Some(self.apply(ty, &HashMap::new())?);
            }
        } else if let Some(want) = expected {
            if let Some(ret) = &func.ret {
                self.infer_field(ret, want, &type_params, &mut solved, span)?;
            }
        }
        let mut checked = Vec::new();
        for (index, (arg, param)) in args.iter().zip(&func.params).enumerate() {
            let param_ty = param.ty.as_ref().unwrap();
            if loose_first && index == 0 {
                let typed = self.check_expr(arg, None)?;
                if let Some(ty) = self.concrete_field(param_ty, &type_params, &solved)? {
                    if !self.ref_compatible(typed.ty, ty) {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                        ));
                    }
                } else {
                    self.loose_receiver = true;
                    self.infer_field(param_ty, typed.ty, &type_params, &mut solved, span)?;
                    self.loose_receiver = false;
                }
                checked.push(typed);
                continue;
            }
            if let Some(ty) = self.concrete_field(param_ty, &type_params, &solved)? {
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
                let typed = self.check_expr(arg, None)?;
                self.infer_field(param_ty, typed.ty, &type_params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{callee}`"),
            ));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let mangled = self.instantiate(callee, &concrete)?;
        let concrete_params = self.sigs[&mangled].params.clone();
        let ret = self.sigs[&mangled].ret.unwrap();
        self.noted_fn(&concrete_params, ret, skip_self);
        if let Some(want) = expected {
            if want != ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: ret,
            kind: TExprKind::Call {
                callee: mangled,
                args: checked,
            },
        })
    }
}
