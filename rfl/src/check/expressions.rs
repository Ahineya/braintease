//! Expressions in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_expr_kind(
        &mut self,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let span = expr.span.start;
        match &expr.kind {
            ExprKind::Int(value) => {
                let ty = match expected {
                    Some(ty) if ty.is_int() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found an integer literal"),
                        ));
                    }
                    None => Ty::I16,
                };
                if !ty.in_range(*value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Int(*value),
                })
            }
            ExprKind::Float(text) => {
                let value = parse_float_literal(self.source, span, text)?;
                let ty = match expected {
                    Some(ty) if ty.is_float() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found a float literal"),
                        ));
                    }
                    None => Ty::F64,
                };
                let bits = float_literal_bits(ty, value)
                    .map_err(|msg| CompileError::at(self.source, span, msg))?;
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Float(bits),
                })
            }
            ExprKind::Bool(value) => {
                if let Some(ty) = expected {
                    if ty != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found bool"),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Bool(*value),
                })
            }
            ExprKind::Name(name) => {
                if self.lookup(name).is_none() {
                    let parent = self
                        .outer_scopes
                        .iter()
                        .rev()
                        .find_map(|scope| scope.get(name).copied());
                    if let Some(parent) = parent {
                        if self.outer_bindings[parent as usize].ty.is_ref() {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                "cannot capture a reference",
                            ));
                        }
                    }
                }
                let Some(id) = self.resolve(name) else {
                    if let Some((ty, value)) = self.consts.get(name).copied() {
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
                        let kind = if ty == Ty::Bool {
                            TExprKind::Bool(value != 0)
                        } else if ty.is_float() {
                            TExprKind::Float(value as u64)
                        } else {
                            TExprKind::Int(value)
                        };
                        return Ok(TExpr { ty, kind });
                    }
                    if let Some(value) = self.fn_value(span, name, expected)? {
                        return Ok(value);
                    }
                    return self.nullary(span, name, expected);
                };
                let ty = self.bindings[id as usize].ty;
                let def = self.bindings[id as usize].def;
                if def.end > def.start {
                    let shown = self.show(ty);
                    self.uses.push(NameUse {
                        span: expr.span,
                        def,
                        def_file: None,
                        ty: shown,
                    });
                }
                self.note_use(span, id, false)?;
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
                    kind: TExprKind::Name(id),
                })
            }
            ExprKind::Borrow {
                mutable,
                temporary,
                expr,
            } => self.check_borrow(span, *mutable, *temporary, expr, expected),
            ExprKind::Deref(inner) => self.check_deref(span, inner, expected),
            ExprKind::Assoc {
                ty_name,
                method,
                args,
            } => {
                let mark = self.borrows.len();
                let result = self.check_assoc(span, ty_name, method, args, expected);
                self.borrows.truncate(mark);
                result
            }
            ExprKind::Try(expr) => self.check_try(span, expr, expected),
            ExprKind::Unary { op, expr: inner } => self.check_unary(span, *op, inner, expected),
            ExprKind::Binary { op, lhs, rhs } => self.check_binary(span, *op, lhs, rhs, expected),
            ExprKind::Char(scalar) => {
                if let Some(want) = expected {
                    if want != Ty::Char {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found char", self.show(want)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::Char,
                    kind: TExprKind::Char(*scalar),
                })
            }
            ExprKind::Str(chars) => {
                let bytes =
                    utf8_bytes(chars).map_err(|msg| CompileError::at(self.source, span, msg))?;
                if let Some(want) = expected {
                    if want != Ty::String {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found String", self.show(want)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::String,
                    kind: TExprKind::Str(bytes),
                })
            }
            ExprKind::CStr(chars) => {
                let ty = self.intern_ptr(false, Ty::I16);
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(want), self.show(ty)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty,
                    kind: TExprKind::CStr(chars.clone()),
                })
            }
            ExprKind::Call {
                callee,
                callee_span,
                type_args,
                args,
            } => {
                let mark = self.borrows.len();
                let result = self.check_call(span, callee, type_args, args, expected)?;
                self.borrows.truncate(mark);
                let written = self.source.get(callee_span.start..callee_span.end);
                let found = self
                    .facts
                    .iter()
                    .rev()
                    .find(|fact| self.source.get(fact.def.start..fact.def.end) == written)
                    .map(|fact| (fact.def, fact.ty.clone()));
                if let Some((def, ty)) = found {
                    self.uses.push(NameUse {
                        span: *callee_span,
                        def,
                        def_file: None,
                        ty,
                    });
                }
                Ok(result)
            }
            ExprKind::Method {
                receiver,
                method,
                method_span,
                resolved,
                args,
            } => {
                let mark = self.borrows.len();
                let result = self.check_method(span, receiver, method, resolved, args, expected);
                self.borrows.truncate(mark);
                if let Some((def, ty)) = self.method_def(method, receiver) {
                    self.uses.push(NameUse {
                        span: *method_span,
                        def,
                        def_file: None,
                        ty,
                    });
                }
                result
            }
            ExprKind::Array(elems) => self.check_array(span, elems, expected),
            ExprKind::Index { base, index } => self.check_index(span, base, index, expected),
            ExprKind::Slice {
                base,
                start,
                end,
                inclusive,
            } => self.check_slice(span, base, start, end, *inclusive, expected),
            ExprKind::Lambda { params, body } => self.check_lambda(span, params, body, expected),
            ExprKind::Match { scrutinee, arms } => {
                self.check_match(span, scrutinee, arms, expected)
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => self.check_if(cond, then_body, else_body, expected),
            ExprKind::While { cond, body } => self.check_while(span, cond, body, expected),
            ExprKind::For { pat, iter, body } => self.check_for(span, pat, iter, body, expected),
            ExprKind::Tuple(fields) => self.check_tuple(span, fields, expected),
            ExprKind::StructLit { name, fields } => {
                self.check_struct_lit(span, name, fields, expected)
            }
            ExprKind::Field { base, name, .. } => self.check_field(span, base, name, expected),
            ExprKind::Range { .. } => Err(CompileError::at(
                self.source,
                span,
                "a range can only be used in `for`",
            )),
            ExprKind::Unsafe(block) => {
                self.unsafe_depth += 1;
                let body = self.check_block(block, expected);
                self.unsafe_depth -= 1;
                let body = body?;
                let ty = body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Block(Box::new(body)),
                })
            }
            ExprKind::Block(block) => {
                let body = self.check_block(block, expected)?;
                let ty = body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Block(Box::new(body)),
                })
            }
            ExprKind::Cast { expr: inner, ty } => {
                let to = self.apply(ty, &HashMap::new())?;
                let inner = self.check_expr(inner, None)?;
                if !can_cast(inner.ty, to) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot cast {} to {to}", inner.ty),
                    ));
                }
                if let Some(folded) = fold_cast(inner.ty, to, &inner.kind) {
                    let folded = folded.map_err(|msg| CompileError::at(self.source, span, msg))?;
                    return Ok(TExpr {
                        ty: to,
                        kind: folded,
                    });
                }
                Ok(TExpr {
                    ty: to,
                    kind: TExprKind::Cast {
                        expr: Box::new(inner),
                        to,
                    },
                })
            }
        }
    }
}
