//! Closures in the RFL type checker.

use super::*;
mod dependencies;

impl<'a> Checker<'a> {
    pub(super) fn check_lambda(
        &mut self,
        span: usize,
        once: bool,
        params: &[crate::ast::Param],
        body: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let expected_sig = match expected {
            Some(Ty::Func(id)) => Some(self.funcs[id as usize].clone()),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found a function", self.show(want)),
                ));
            }
            None => None,
        };
        if let Some(sig) = &expected_sig {
            if sig.once != once {
                return Err(CompileError::at(self.source, span, "consuming and reusable functions have different types; use `once fn` for a consuming callback"));
            }
            if sig.params.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {} parameters, found {}",
                        sig.params.len(),
                        params.len()
                    ),
                ));
            }
        }
        let mut sig_params = Vec::new();
        for (index, param) in params.iter().enumerate() {
            let ty = if let Some(parsed) = &param.ty {
                let ty = self.apply(parsed, &HashMap::new())?;
                if let Some(sig) = &expected_sig {
                    if sig.params[index] != ty {
                        return Err(CompileError::at(
                            self.source,
                            param.name_span.start,
                            format!(
                                "expected {}, `{}` has type {}",
                                self.show(sig.params[index]),
                                param.name,
                                self.show(ty)
                            ),
                        ));
                    }
                }
                ty
            } else if let Some(sig) = &expected_sig {
                sig.params[index]
            } else {
                Ty::I16
            };
            sig_params.push(ty);
        }
        // Resolve transitive captures in the enclosing frame before suspending
        // its scopes. Parameters and local shadowing are respected by the walk.
        for (name, at) in dependencies::free_names(params, body) {
            if self.lookup(&name).is_none() {
                if let Some(parent) = self
                    .outer_scopes
                    .iter()
                    .rev()
                    .find_map(|s| s.get(&name).copied())
                {
                    if self.outer_bindings[parent as usize].ty.is_ref() {
                        return Err(CompileError::at(
                            self.source,
                            at,
                            "cannot capture a reference",
                        ));
                    }
                    self.resolve(&name);
                }
            }
        }
        let name = format!("__lambda{}", self.lambda_count);
        self.lambda_count += 1;
        let declared_ret = expected_sig.as_ref().map(|sig| sig.ret);
        self.sigs.insert(
            name.clone(),
            Sig {
                params: sig_params.clone(),
                ret: declared_ret,
                c_abi: false,
                variadic: false,
            },
        );
        let block = match &body.kind {
            ExprKind::Block(block) => (**block).clone(),
            _ => Block {
                span: body.span,
                stmts: Vec::new(),
                tail: Some(body.clone()),
            },
        };
        let func = Function {
            is_async: false,
            name: name.clone(),
            name_span: crate::ast::Span::new(span, span),
            type_params: Vec::new(),
            params: params.to_vec(),
            ret: None,
            body: block,
            c_export: false,
            exported: false,
        };
        let saved_once = std::mem::replace(&mut self.capture_once, once);
        let checked = self.check_fn(&func, &name, true, true);
        self.capture_once = saved_once;
        checked?;
        let captures = self
            .typed
            .iter()
            .find(|f| f.name == name)
            .unwrap()
            .captures
            .clone();
        for cap in captures {
            if cap.transfer {
                self.note_use(span, cap.parent, true)?;
            }
        }
        let ret = self.sigs[&name].ret.unwrap();
        let ty = self.intern_func_mode(sig_params, ret, once);
        Ok(TExpr {
            ty,
            kind: TExprKind::Closure { func: name },
        })
    }
}
