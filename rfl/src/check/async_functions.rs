//! The public future handle has one compiler-private, erased owner field.
use super::*;

impl Checker<'_> {
    pub(super) fn check_async_builtin(
        &mut self,
        span: usize,
        name: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let arity = if name == "__async_poll" { 2 } else { 1 };
        if args.len() != arity {
            return Err(CompileError::at(
                self.source,
                span,
                "async operation needs a mutable future reference and, for polling, a context",
            ));
        }
        let future = self.check_expr(&args[0], None)?;
        let output = match future.ty {
            Ty::Ref(id) if self.refs[id as usize].mutable => match self.refs[id as usize].inner {
                Ty::Adt(id) if self.schemas[self.adts[id as usize].schema].name == "Async" => {
                    Some(self.adts[id as usize].args[0])
                }
                _ => None,
            },
            _ => None,
        }
        .ok_or_else(|| {
            CompileError::at(self.source, span, "async operation needs &mut Async<T>")
        })?;
        self.note_reference_use(span, &future, Some(future.ty))?;
        if name == "__async_cancel" {
            if expected.is_some_and(|ty| ty != Ty::Unit) {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "async cancellation returns unit",
                ));
            }
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::AsyncCancel {
                    future: Box::new(future),
                },
            });
        }
        let poll = expected
            .filter(|ty| match ty {
                Ty::Adt(id) => {
                    let adt = &self.adts[*id as usize];
                    self.schemas[adt.schema].name.rsplit('$').next() == Some("Poll")
                        && adt.args == [output]
                        && adt.ctors.len() == 2
                        && adt.ctors[0].fields.is_empty()
                        && adt.ctors[1].fields == [output]
                }
                _ => false,
            })
            .ok_or_else(|| {
                CompileError::at(
                    self.source,
                    span,
                    "async polling needs a Poll<T> result context",
                )
            })?;
        let context = self.check_expr(&args[1], None)?;
        let valid = match context.ty {
            Ty::Ref(id) if self.refs[id as usize].mutable => match self.refs[id as usize].inner {
                Ty::Struct { id, .. } => {
                    let name = &self.structs[id as usize].name;
                    name.rsplit('$').next() == Some("Context")
                        && self
                            .type_defs
                            .get(name)
                            .is_some_and(|span| self.core_definition(*span))
                }
                _ => false,
            },
            _ => false,
        };
        if !valid {
            return Err(CompileError::at(
                self.source,
                span,
                "async polling needs &mut Context",
            ));
        }
        self.note_reference_use(span, &context, Some(context.ty))?;
        Ok(TExpr {
            ty: poll,
            kind: TExprKind::AsyncPoll {
                future: Box::new(future),
                context: Box::new(context),
            },
        })
    }

    pub(super) fn check_await(
        &mut self,
        span: usize,
        child: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if self.async_context.is_none() {
            return Err(CompileError::at(
                self.source,
                span,
                "await is only allowed inside an async function",
            ));
        }
        // Initial suspension rules are deliberately lexical: closing a borrow's
        // block proves it ended. Raw pointers still require the unsafe contract.
        if !self.borrows.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                "a borrowed reference cannot cross await; end its block before suspending",
            ));
        }
        let child = self.check_expr(child, None)?;
        self.require_vec_elem(span, child.ty)?;
        let candidates = self.trait_candidates(span, child.ty, "poll")?;
        let tr = candidates
            .into_iter()
            .find(|name| {
                name.rsplit('$').next() == Some("Future")
                    && self
                        .traits
                        .iter()
                        .find(|tr| tr.name == *name)
                        .is_some_and(|tr| self.core_definition(tr.span))
            })
            .ok_or_else(|| {
                CompileError::at(
                    self.source,
                    span,
                    "await needs a Future value and the std/future Future trait in scope",
                )
            })?;
        let signature = self.projected_method_signature(&tr, "poll", child.ty)?;
        let Ty::Adt(poll_id) = signature.ret else {
            return Err(CompileError::at(
                self.source,
                span,
                "Future::poll must return Poll<T>",
            ));
        };
        let poll_def = &self.adts[poll_id as usize];
        if poll_def.args.len() != 1
            || poll_def.ctors.len() != 2
            || !poll_def.ctors[0].fields.is_empty()
            || poll_def.ctors[1].fields != poll_def.args
            || signature.params.len() != 1
        {
            return Err(CompileError::at(
                self.source,
                span,
                "Future::poll must return Poll<T> and take &mut Context",
            ));
        }
        let output = poll_def.args[0];
        if expected.is_some_and(|ty| ty != output) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("await returns {}", self.show(output)),
            ));
        }
        let context_ty = signature.params[0];
        if !matches!(context_ty, Ty::Ref(id) if self.refs[id as usize].mutable) {
            return Err(CompileError::at(
                self.source,
                span,
                "Future::poll needs a mutable context reference",
            ));
        }
        if let Some(id) = self.async_context.flatten() {
            if self.bindings[id as usize].ty != context_ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "awaited futures must use the same Context type",
                ));
            }
        } else {
            let id = self.bind("$async$context".into(), context_ty, false);
            // The first await may be nested in a branch or loop. This hidden
            // binding belongs to the function, rather than that lexical block.
            self.scopes.last_mut().unwrap().remove("$async$context");
            self.scopes[0].insert("$async$context".into(), id);
            self.async_context = Some(Some(id));
        }
        // Synthetic receiver/cancel calls must not replace hover, signature or
        // name facts at the source's awaited expression.
        let at = Span::empty();
        let source_span = std::mem::replace(&mut self.current_span, Span::empty());
        let name = format!("$async$cleanup${}", self.lambda_count);
        self.lambda_count += 1;
        let reference = self.intern_ref(true, child.ty);
        let cleanup_ty = self.intern_func(vec![reference], Ty::Unit);
        let unit = ParsedTy::Named {
            span: at,
            name: "unit".into(),
            args: vec![],
        };
        let parameter = "$async$child".to_string();
        let cleanup = Function {
            is_async: false,
            name: name.clone(),
            name_span: Span::new(span, span),
            type_params: vec![],
            params: vec![Param {
                name: parameter.clone(),
                name_span: at,
                ty: Some(self.ty_to_parsed(reference)),
                pat: Pat::Name {
                    name: parameter.clone(),
                    span: at,
                },
            }],
            ret: Some(unit),
            c_export: false,
            exported: false,
            body: Block {
                span: at,
                stmts: vec![Stmt::Expr(Expr {
                    span: at,
                    kind: ExprKind::Assoc {
                        ty_name: tr.clone(),
                        method: "cancel".into(),
                        args: vec![Expr {
                            span: at,
                            kind: ExprKind::Name(parameter),
                        }],
                    },
                })],
                tail: None,
            },
        };
        self.sigs.insert(
            name.clone(),
            Sig {
                params: vec![reference],
                ret: Some(Ty::Unit),
                c_abi: false,
                variadic: false,
            },
        );
        self.check_fn(&cleanup, &name, true, false)?;
        let owner_ty = self.intern_owned(child.ty);
        let owner = TExpr {
            ty: owner_ty,
            kind: TExprKind::Owned {
                value: Box::new(child),
                cleanup: Box::new(TExpr {
                    ty: cleanup_ty,
                    kind: TExprKind::Closure { func: name },
                }),
            },
        };
        let local_name = format!("$async$await${}", self.bindings.len());
        let binding = self.bind(local_name.clone(), owner_ty, true);
        let receiver = Expr {
            span: at,
            kind: ExprKind::Method {
                receiver: Box::new(Expr {
                    span: at,
                    kind: ExprKind::Name(local_name),
                }),
                method: "borrow_mut".into(),
                method_span: at,
                resolved: "borrow_mut".into(),
                args: vec![],
            },
        };
        let mark = self.borrows.len();
        let poll = self.check_trait_method(
            span,
            &tr,
            &receiver,
            reference,
            "poll",
            &[Expr {
                span: at,
                kind: ExprKind::Name("$async$context".into()),
            }],
            Some(signature.ret),
        )?;
        self.borrows.truncate(mark);
        self.current_span = source_span;
        Ok(TExpr {
            ty: output,
            kind: TExprKind::Await {
                owner: Box::new(owner),
                binding,
                poll: Box::new(poll),
            },
        })
    }

    fn core_definition(&self, span: Span) -> bool {
        self.trait_scopes
            .iter()
            .any(|scope| scope.core && scope.span.contains(span.start))
    }
}
