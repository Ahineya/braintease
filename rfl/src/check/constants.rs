//! Constants in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn eval_consts(&mut self, consts: &[crate::ast::Const]) -> Result<(), CompileError> {
        for item in consts {
            if self.consts.contains_key(&item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate constant `{}`", item.name),
                ));
            }
            if self.ast_fns.iter().any(|func| func.name == item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("`{}` is already a function", item.name),
                ));
            }
            let ty = self.apply(&item.ty, &HashMap::new())?;
            if !ty.is_int() && !ty.is_float() && ty != Ty::Bool {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!(
                        "a constant must be a number or bool, found {}",
                        self.show(ty)
                    ),
                ));
            }
            let (got, value) = self.eval_const(&item.init, Some(ty))?;
            if got != ty {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!(
                        "`{}` has type {}, the initializer has type {}",
                        item.name,
                        self.show(ty),
                        self.show(got)
                    ),
                ));
            }
            self.consts.insert(item.name.clone(), (ty, value));
        }
        Ok(())
    }

    pub(super) fn const_hint(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Int(_) | ExprKind::Float(_) => None,
            ExprKind::Bool(_) => Some(Ty::Bool),
            ExprKind::Name(name) => self.consts.get(name).map(|(ty, _)| *ty),
            ExprKind::Unary {
                op: UnaryOp::Neg | UnaryOp::BitNot,
                expr,
            } => self.const_hint(expr),
            ExprKind::Unary {
                op: UnaryOp::Not, ..
            } => Some(Ty::Bool),
            ExprKind::Binary { op, .. } if op.is_compare() || op.is_logic() => Some(Ty::Bool),
            ExprKind::Binary { lhs, rhs, .. } => {
                self.const_hint(lhs).or_else(|| self.const_hint(rhs))
            }
            _ => None,
        }
    }

    pub(super) fn eval_const(
        &self,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<(Ty, i64), CompileError> {
        let span = expr.span.start;
        match &expr.kind {
            ExprKind::Int(value) => {
                let ty = match expected {
                    Some(ty) if ty.is_int() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found an integer literal", self.show(ty)),
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
                Ok((ty, *value))
            }
            ExprKind::Float(text) => {
                let value = parse_float_literal(self.source, span, text)?;
                let ty = match expected {
                    Some(ty) if ty.is_float() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found a float literal", self.show(ty)),
                        ));
                    }
                    None => Ty::F64,
                };
                let bits = float_literal_bits(ty, value)
                    .map_err(|msg| CompileError::at(self.source, span, msg))?;
                Ok((ty, bits as i64))
            }
            ExprKind::Bool(value) => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                Ok((Ty::Bool, i64::from(*value)))
            }
            ExprKind::Name(name) => {
                let Some((ty, value)) = self.consts.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("unknown constant `{name}`"),
                    ));
                };
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
                Ok((ty, value))
            }
            ExprKind::Unary {
                op: UnaryOp::Neg,
                expr: inner,
            } => {
                let (ty, value) =
                    self.eval_const(inner, expected.filter(|ty| ty.is_int() || ty.is_float()))?;
                if ty.is_float() {
                    let sign = if ty == Ty::F32 {
                        0x8000_0000u64
                    } else {
                        1u64 << 63
                    };
                    return Ok((ty, (value as u64 ^ sign) as i64));
                }
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot negate {}", self.show(ty)),
                    ));
                }
                let Some(value) = value.checked_neg() else {
                    return Err(CompileError::at(self.source, span, "constant overflow"));
                };
                if !ty.in_range(value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok((ty, value))
            }
            ExprKind::Unary {
                op: UnaryOp::Not,
                expr: inner,
            } => {
                let (ty, value) = self.eval_const(inner, Some(Ty::Bool))?;
                if ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "`!` in a constant expects bool",
                    ));
                }
                Ok((Ty::Bool, i64::from(value == 0)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_logic() => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                let (left_ty, left) = self.eval_const(lhs, Some(Ty::Bool))?;
                let (right_ty, right) = self.eval_const(rhs, Some(Ty::Bool))?;
                if left_ty != Ty::Bool || right_ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "`&&` and `||` expect bool",
                    ));
                }
                let value = match op {
                    BinOp::And => left != 0 && right != 0,
                    BinOp::Or => left != 0 || right != 0,
                    _ => false,
                };
                Ok((Ty::Bool, i64::from(value)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_compare() => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                let operand = self
                    .const_hint(lhs)
                    .filter(|ty| ty.is_int() || ty.is_float())
                    .or_else(|| {
                        self.const_hint(rhs)
                            .filter(|ty| ty.is_int() || ty.is_float())
                    })
                    .unwrap_or_else(|| number_default(lhs, rhs));
                let (left_ty, left) = self.eval_const(lhs, Some(operand))?;
                let (right_ty, right) = self.eval_const(rhs, Some(operand))?;
                if left_ty != operand || right_ty != operand {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "cannot compare {} and {}",
                            self.show(left_ty),
                            self.show(right_ty)
                        ),
                    ));
                }
                if operand.is_float() {
                    let value = float_compare(operand, *op, left as u64, right as u64);
                    return Ok((Ty::Bool, i64::from(value)));
                }
                let value = match op {
                    BinOp::Eq => left == right,
                    BinOp::Ne => left != right,
                    BinOp::Lt => const_less(operand, left, right),
                    BinOp::Le => !const_less(operand, right, left),
                    BinOp::Gt => const_less(operand, right, left),
                    BinOp::Ge => !const_less(operand, left, right),
                    _ => false,
                };
                Ok((Ty::Bool, i64::from(value)))
            }
            ExprKind::Unary {
                op: UnaryOp::BitNot,
                expr: inner,
            } => {
                let (ty, value) = self.eval_const(inner, expected.filter(|ty| ty.is_int()))?;
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`~` expects an integer, found {}", self.show(ty)),
                    ));
                }
                Ok((ty, fold_bitnot(ty, value)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_bitwise() => {
                let ty = expected
                    .filter(|ty| ty.is_int())
                    .or_else(|| self.const_hint(lhs))
                    .or_else(|| self.const_hint(rhs))
                    .unwrap_or_else(|| number_default(lhs, rhs));
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("operator expects an integer, found {}", self.show(ty)),
                    ));
                }
                let (_, left) = self.eval_const(lhs, Some(ty))?;
                let (_, right) = self.eval_const(rhs, Some(ty))?;
                Ok((ty, fold_bitwise(ty, *op, left, right)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_arith() => {
                let ty = expected
                    .filter(|ty| ty.is_int() || ty.is_float())
                    .or_else(|| self.const_hint(lhs))
                    .or_else(|| self.const_hint(rhs))
                    .unwrap_or_else(|| number_default(lhs, rhs));
                if ty.is_float() {
                    if *op == BinOp::Rem {
                        return Err(CompileError::at(self.source, span, "`%` expects integers"));
                    }
                    let (_, left) = self.eval_const(lhs, Some(ty))?;
                    let (_, right) = self.eval_const(rhs, Some(ty))?;
                    return Ok((ty, float_arith(ty, *op, left as u64, right as u64) as i64));
                }
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("operator expects a number, found {}", self.show(ty)),
                    ));
                }
                let (_, left) = self.eval_const(lhs, Some(ty))?;
                let (_, right) = self.eval_const(rhs, Some(ty))?;
                let value = match op {
                    BinOp::Add => left.checked_add(right),
                    BinOp::Sub => left.checked_sub(right),
                    BinOp::Mul => left.checked_mul(right),
                    BinOp::Div if right == 0 => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "division by zero in a constant",
                        ))
                    }
                    BinOp::Rem if right == 0 => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "division by zero in a constant",
                        ))
                    }
                    BinOp::Div => left.checked_div(right),
                    BinOp::Rem => left.checked_rem(right),
                    _ => None,
                };
                let Some(value) = value else {
                    return Err(CompileError::at(self.source, span, "constant overflow"));
                };
                if !ty.in_range(value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok((ty, value))
            }
            _ => Err(CompileError::at(
                self.source,
                span,
                "not a constant expression",
            )),
        }
    }
}
