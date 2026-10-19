//! Preserve solved input constraints for callbacks, vector literals and variants
//! without inventing types for unsolved slots. Ordinary expression checking still
//! runs once and enforces explicit arguments, cardinality and ownership.
use super::*;

impl Checker<'_> {
    pub(in crate::check) fn check_inferred_argument(
        &mut self,
        argument: &Expr,
        field: &ParsedTy,
        params: &[String],
        solved: &[Option<Ty>],
    ) -> Result<TExpr, CompileError> {
        // The output slot may still be unknown even though earlier arguments
        // have solved every callback input. Preserve those input hints instead
        // of checking an unannotated lambda with the default i16 parameters.
        if let (
            ExprKind::Lambda {
                params: written, ..
            },
            ParsedTy::Func { params: inputs, .. },
        ) = (&argument.kind, field)
        {
            if written.len() != inputs.len() {
                return Err(CompileError::at(
                    self.source,
                    argument.span.start,
                    format!(
                        "expected {} parameters, found {}",
                        inputs.len(),
                        written.len()
                    ),
                ));
            }
            let mut contextual = argument.clone();
            let ExprKind::Lambda {
                params: annotated, ..
            } = &mut contextual.kind
            else {
                unreachable!()
            };
            for (param, input) in annotated.iter_mut().zip(inputs) {
                if param.ty.is_none() {
                    if let Some(ty) = self.concrete_field(input, params, solved)? {
                        param.ty = Some(self.ty_to_parsed(ty));
                    }
                }
            }
            return self.check_expr(&contextual, None);
        }
        // A named monomorphic function has no owned environment. Adapt its
        // callable cardinality even when a generic result slot is still unknown.
        if matches!(field, ParsedTy::Func { once: true, .. }) {
            if let ExprKind::Name(name) = &argument.kind {
                if self.binding_ty(name).is_none() && self.sigs.contains_key(name) {
                    let mut value = self.check_expr(argument, None)?;
                    if let Ty::Func(id) = value.ty {
                        let sig = self.funcs[id as usize].clone();
                        value.ty = self.intern_func_mode(sig.params, sig.ret, true);
                    }
                    return Ok(value);
                }
            }
        }
        let ParsedTy::Named { name, args, .. } = field else {
            return self.check_expr(argument, None);
        };
        let unknown_head = params
            .iter()
            .position(|p| p == name)
            .is_some_and(|index| solved[index].is_none());
        if unknown_head && args.len() == 1 && matches!(&argument.kind, ExprKind::Array(_)) {
            // A vector literal identifies the complete unary head; no partial
            // constructor slot is guessed from its element type.
            if let Some(elem) = self.concrete_field(&args[0], params, solved)? {
                let ty = self.intern_vec(elem);
                return self.check_expr(argument, Some(ty));
            }
        }
        let (callee, callee_span, type_args, values) = match &argument.kind {
            ExprKind::Call {
                callee,
                callee_span,
                type_args,
                args,
            } if self.binding_ty(callee).is_none() => (
                callee.as_str(),
                *callee_span,
                type_args.as_slice(),
                args.as_slice(),
            ),
            ExprKind::Name(name) if self.binding_ty(name).is_none() => {
                (name.as_str(), argument.span, &[][..], &[][..])
            }
            _ => return self.check_expr(argument, None),
        };
        let Some(&(schema, _)) = self.ctor_schema.get(callee) else {
            return self.check_expr(argument, None);
        };
        let subst: HashMap<_, _> = params
            .iter()
            .zip(solved)
            .filter_map(|(name, ty)| ty.map(|ty| (name.clone(), ty)))
            .collect();
        let slot = || ParsedTy::Named {
            name: "_".into(),
            args: Vec::new(),
            span: Span::empty(),
        };
        let head = subst
            .get(name)
            .or_else(|| self.subst.get(name))
            .copied()
            .or_else(|| {
                if name == "Self" {
                    self.assoc_self.as_ref().map(|(_, ty)| *ty)
                } else {
                    None
                }
            });
        let mut hints = Vec::new();
        let infer_head = unknown_head
            && self
                .constructor_names
                .contains_key(&self.schemas[schema].name);
        if infer_head && !args.is_empty() && args.len() != self.schemas[schema].type_params.len() {
            return Err(CompileError::at(self.source, argument.span.start,
                "cannot infer a partial constructor; supply explicit slots such as `Result<_, String>`"));
        }
        if let Some(head @ Ty::Constructor(_)) = head {
            let (base, fixed) = self.constructor_parts(head).unwrap();
            let Ty::Constructor(id) = base else {
                unreachable!()
            };
            if self.constructors[id as usize].name != self.schemas[schema].name
                || args.len() != fixed.iter().filter(|ty| ty.is_none()).count()
            {
                return self.check_expr(argument, None);
            }
            let mut args = args.iter();
            for fixed in fixed {
                hints.push(match fixed {
                    Some(ty) => self.ty_to_parsed(ty),
                    None => self
                        .concrete_field(args.next().unwrap(), params, solved)?
                        .map(|ty| self.ty_to_parsed(ty))
                        .unwrap_or_else(slot),
                });
            }
        } else {
            if (name != &self.schemas[schema].name && !infer_head)
                || args.len() != self.schemas[schema].type_params.len()
            {
                return self.check_expr(argument, None);
            }
            let kinds = self.schemas[schema].type_params.clone();
            for (arg, kind) in args.iter().zip(kinds) {
                hints.push(if mentions_unsolved(arg, params, solved) {
                    slot()
                } else {
                    let ty = self.apply_argument(arg, kind.arity, &subst)?;
                    self.ty_to_parsed(ty)
                });
            }
        }
        if hints.iter().all(kinds::is_slot)
            || !type_args.is_empty() && type_args.len() != hints.len()
        {
            return self.check_expr(argument, None);
        }
        if !type_args.is_empty() {
            for (hint, explicit) in hints.iter_mut().zip(type_args) {
                if !kinds::is_slot(explicit) {
                    *hint = explicit.clone();
                }
            }
        }
        let contextual = Expr {
            span: argument.span,
            kind: ExprKind::Call {
                callee: callee.into(),
                callee_span,
                type_args: hints,
                args: values.to_vec(),
            },
        };
        self.check_expr(&contextual, None)
    }
}
