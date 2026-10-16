//! Operators in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_unary(
        &mut self,
        span: usize,
        op: UnaryOp,
        inner: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        match op {
            UnaryOp::Not => {
                let inner = self.check_expr(inner, Some(Ty::Bool))?;
                if inner.ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`!` expects bool, found {}", inner.ty),
                    ));
                }
                Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
            UnaryOp::Neg => {
                let want = expected.filter(|ty| ty.is_int() || ty.is_float());
                // Check the signed literal as a whole: its positive magnitude
                // need not fit (for example -32768 in i16 or -128 in i8).
                if let ExprKind::Int(value) = &inner.kind {
                    let ty = want.unwrap_or(Ty::I16);
                    if ty.is_signed() {
                        let value =
                            value
                                .checked_neg()
                                .filter(|v| ty.in_range(*v))
                                .ok_or_else(|| {
                                    CompileError::at(
                                        self.source,
                                        span,
                                        format!("-{value} does not fit in {ty}"),
                                    )
                                })?;
                        return Ok(TExpr {
                            ty,
                            kind: TExprKind::Int(value),
                        });
                    }
                }
                let inner = self.check_expr(inner, want)?;
                if inner.ty.is_float() {
                    if let TExprKind::Float(bits) = inner.kind {
                        let sign = if inner.ty == Ty::F32 {
                            0x8000_0000
                        } else {
                            1u64 << 63
                        };
                        return Ok(TExpr {
                            ty: inner.ty,
                            kind: TExprKind::Float(bits ^ sign),
                        });
                    }
                    return Ok(TExpr {
                        ty: inner.ty,
                        kind: TExprKind::Unary {
                            op,
                            expr: Box::new(inner),
                        },
                    });
                }
                if !inner.ty.is_int() || !inner.ty.is_signed() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "`-` expects a signed integer or a float, found {}",
                            inner.ty
                        ),
                    ));
                }
                if let TExprKind::Int(value) = inner.kind {
                    let Some(negated) = value.checked_neg() else {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("-{value} does not fit in {}", inner.ty),
                        ));
                    };
                    if !inner.ty.in_range(negated) {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("{negated} does not fit in {}", inner.ty),
                        ));
                    }
                    return Ok(TExpr {
                        ty: inner.ty,
                        kind: TExprKind::Int(negated),
                    });
                }
                Ok(TExpr {
                    ty: inner.ty,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
            UnaryOp::BitNot => {
                let want = expected.filter(|ty| ty.is_int());
                let inner = self.check_expr(inner, want)?;
                if !inner.ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`~` expects an integer, found {}", self.show(inner.ty)),
                    ));
                }
                Ok(TExpr {
                    ty: inner.ty,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
        }
    }

    pub(super) fn check_binary(
        &mut self,
        span: usize,
        op: BinOp,
        lhs: &Expr,
        rhs: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if op.is_logic() {
            let lhs = self.check_expr(lhs, Some(Ty::Bool))?;
            let rhs = self.check_expr(rhs, Some(Ty::Bool))?;
            if lhs.ty != Ty::Bool || rhs.ty != Ty::Bool {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "`&&` and `||` expect bool",
                ));
            }
            return Ok(TExpr {
                ty: Ty::Bool,
                kind: TExprKind::Binary {
                    op,
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if op == BinOp::Add
            && (self.peek(lhs) == Some(Ty::String) || self.peek(rhs) == Some(Ty::String))
        {
            let lhs = self.check_expr(lhs, Some(Ty::String))?;
            let rhs = self.check_expr(rhs, Some(Ty::String))?;
            if lhs.ty != Ty::String || rhs.ty != Ty::String {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "cannot combine {} and {}",
                        self.show(lhs.ty),
                        self.show(rhs.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: Ty::String,
                kind: TExprKind::StrConcat {
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if op.is_bitwise() {
            let operand_ty = expected
                .filter(|ty| ty.is_int())
                .or_else(|| self.peek(lhs))
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs));
            if !operand_ty.is_int() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "operator expects an integer, found {}",
                        self.show(operand_ty)
                    ),
                ));
            }
            let lhs = self.check_expr(lhs, Some(operand_ty))?;
            let rhs = self.check_expr(rhs, Some(operand_ty))?;
            if lhs.ty != operand_ty || rhs.ty != operand_ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "cannot combine {} and {}",
                        self.show(lhs.ty),
                        self.show(rhs.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: operand_ty,
                kind: TExprKind::Binary {
                    op,
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if matches!(op, BinOp::Eq | BinOp::Ne) || op.is_compare() {
            let hint = self.peek(lhs).or_else(|| self.peek(rhs));
            if hint == Some(Ty::String) || hint == Some(Ty::Char) {
                if hint == Some(Ty::String) && !matches!(op, BinOp::Eq | BinOp::Ne) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "strings can only be compared with `==` and `!=`",
                    ));
                }
                if hint == Some(Ty::Char) && !op.is_compare() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "a char cannot be used in arithmetic",
                    ));
                }
                let lhs = self.check_expr(lhs, hint)?;
                let rhs = self.check_expr(rhs, Some(lhs.ty))?;
                if lhs.ty != rhs.ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot combine {} and {}", lhs.ty, rhs.ty),
                    ));
                }
                return Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Binary {
                        op,
                        lhs: Box::new(lhs),
                        rhs: Box::new(rhs),
                    },
                });
            }
        }
        let operand_ty = if op.is_compare() {
            self.peek(lhs)
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs))
        } else {
            expected
                .filter(|ty| ty.is_int() || ty.is_float())
                .or_else(|| self.peek(lhs))
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs))
        };
        if operand_ty.is_float() {
            if op == BinOp::Rem {
                return Err(CompileError::at(self.source, span, "`%` expects integers"));
            }
        } else if !operand_ty.is_int()
            && !(self.generic_check && self.opaque.contains_key(&operand_ty))
        {
            return Err(CompileError::at(
                self.source,
                span,
                format!("operator expects a number, found {operand_ty}"),
            ));
        }
        let lhs = self.check_expr(lhs, Some(operand_ty))?;
        let rhs = self.check_expr(rhs, Some(operand_ty))?;
        if lhs.ty != operand_ty || rhs.ty != operand_ty {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot combine {} and {}", lhs.ty, rhs.ty),
            ));
        }
        let ty = if op.is_compare() {
            Ty::Bool
        } else {
            operand_ty
        };
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, this expression has type {ty}"),
                ));
            }
        }
        if operand_ty.is_float() {
            if let (TExprKind::Float(left), TExprKind::Float(right)) = (&lhs.kind, &rhs.kind) {
                let left = *left;
                let right = *right;
                if op.is_compare() {
                    return Ok(TExpr {
                        ty: Ty::Bool,
                        kind: TExprKind::Bool(float_compare(operand_ty, op, left, right)),
                    });
                }
                let bits = float_arith(operand_ty, op, left, right);
                return Ok(TExpr {
                    ty,
                    kind: TExprKind::Float(bits),
                });
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Binary {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            },
        })
    }
}
