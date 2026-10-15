//! Control in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_builtin(
        &mut self,
        span: usize,
        callee: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if callee == "from_char" || callee == "from_byte" {
            let arg_ty = if callee == "from_char" { "char" } else { "u8" };
            self.note_sig(format!("fn({arg_ty}) -> String"));
            if args.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects 1 argument, found {}", args.len()),
                ));
            }
            let want_arg = if callee == "from_char" {
                Ty::Char
            } else {
                Ty::U8
            };
            let arg = self.check_expr(&args[0], Some(want_arg))?;
            if arg.ty != want_arg {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {want_arg}, found {}", self.show(arg.ty)),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::String {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `{callee}` has type String", self.show(want)),
                    ));
                }
            }
            let kind = if callee == "from_char" {
                TExprKind::StrFromChar(Box::new(arg))
            } else {
                TExprKind::StrFromByte(Box::new(arg))
            };
            return Ok(TExpr {
                ty: Ty::String,
                kind,
            });
        }
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `{callee}` has type unit"),
                ));
            }
        }
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects 1 argument, found {}", args.len()),
            ));
        }
        let newline = callee == "println";
        self.note_sig("fn(integer | bool | char | float | String) -> ()".to_string());
        if callee == "putc" {
            self.note_sig("fn(integer | char) -> ()".to_string());
            let arg = self.check_expr(&args[0], None)?;
            if !arg.ty.is_int() && arg.ty != Ty::Char {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`putc` expects an integer or char, found {}",
                        self.show(arg.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::Putc(Box::new(arg)),
            });
        }
        if let ExprKind::Str(chars) = &args[0].kind {
            let bytes =
                utf8_bytes(chars).map_err(|msg| CompileError::at(self.source, span, msg))?;
            let cells = bytes.into_iter().map(u16::from).collect();
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::PrintLit {
                    chars: cells,
                    newline,
                },
            });
        }
        let arg = self.check_expr(&args[0], None)?;
        if !arg.ty.is_int()
            && !arg.ty.is_float()
            && arg.ty != Ty::Bool
            && arg.ty != Ty::Char
            && arg.ty != Ty::String
        {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects an integer, float, bool, char, or string, found {}",
                    self.show(arg.ty)
                ),
            ));
        }
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Print {
                value: Box::new(arg),
                newline,
            },
        })
    }

    pub(super) fn check_if(
        &mut self,
        cond: &Expr,
        then_body: &Block,
        else_body: &Option<Box<Expr>>,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        let Some(else_body) = else_body else {
            let then_body = self.check_block(then_body, None)?;
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::If {
                    cond: Box::new(cond),
                    then_body: Box::new(then_body),
                    else_body: None,
                },
            });
        };
        let then_body = self.check_block(then_body, expected)?;
        let then_ty = then_body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
        let else_expr = self.check_expr(else_body, Some(then_ty))?;
        if else_expr.ty != then_ty {
            return Err(CompileError::at(
                self.source,
                else_body.span.start,
                format!("if branches have types {then_ty} and {}", else_expr.ty),
            ));
        }
        Ok(TExpr {
            ty: then_ty,
            kind: TExprKind::If {
                cond: Box::new(cond),
                then_body: Box::new(then_body),
                else_body: Some(Box::new(else_expr)),
            },
        })
    }

    pub(super) fn check_while(
        &mut self,
        span: usize,
        cond: &Expr,
        body: &Block,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `while` has type unit"),
                ));
            }
        }
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        if cond.ty != Ty::Bool {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`while` expects bool, found {}", cond.ty),
            ));
        }
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::While {
                cond: Box::new(cond),
                body: Box::new(body),
            },
        })
    }

    /// A monomorphic function name is a callable value: code id plus an empty environment.
    /// Every function frame reserves the environment slot, so the same call protocol reaches
    /// named functions and lambdas.
    pub(super) fn fn_value(
        &mut self,
        span: usize,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<Option<TExpr>, CompileError> {
        if self.fn_schemas.contains_key(name) {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "generic function `{name}` needs a concrete signature to be used as a value"
                ),
            ));
        }
        let Some(sig) = self.sigs.get(name) else {
            return Ok(None);
        };
        if sig.c_abi {
            if sig.variadic {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("variadic function `{name}` cannot be used as a value"),
                ));
            }
            let Some(ret) = sig.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{name}` needs a return type to be used as a value"),
                ));
            };
            let params = sig.params.clone();
            let ty = self.intern_cfn(params, ret);
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
            return Ok(Some(TExpr {
                ty,
                kind: TExprKind::CFn {
                    symbol: name.to_string(),
                },
            }));
        }
        let Some(ret) = sig.ret else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("function `{name}` needs a return type to be used as a value"),
            ));
        };
        let params = sig.params.clone();
        let ty = self.intern_func(params, ret);
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
        Ok(Some(TExpr {
            ty,
            kind: TExprKind::Closure {
                func: name.to_string(),
            },
        }))
    }

    pub(super) fn check_try(
        &mut self,
        span: usize,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let operand = self.check_expr(expr, None)?;
        let Ty::Adt(id) = operand.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`?` expects Option or Result, found {}",
                    self.show(operand.ty)
                ),
            ));
        };
        let schema = self.adts[id as usize].schema;
        let name = self.schemas[schema].name.clone();
        let args = self.adts[id as usize].args.clone();
        let (payload, fail, ok_tag, fail_tag) = if name == "Option" {
            let Ty::Adt(ret_id) = self.fn_ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Option, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            };
            if self.schemas[self.adts[ret_id as usize].schema].name != "Option" {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Option, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            }
            (
                args[0],
                None,
                self.ctor_schema["Some"].1,
                self.ctor_schema["None"].1,
            )
        } else if name == "Result" {
            let Ty::Adt(ret_id) = self.fn_ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Result, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            };
            if self.schemas[self.adts[ret_id as usize].schema].name != "Result" {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Result, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            }
            let ret_err = self.adts[ret_id as usize].args[1];
            if ret_err != args[1] {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` error type is {}, the function returns {}",
                        self.show(args[1]),
                        self.show(ret_err)
                    ),
                ));
            }
            (
                args[0],
                Some(args[1]),
                self.ctor_schema["Ok"].1,
                self.ctor_schema["Err"].1,
            )
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                "`?` expects Option or Result",
            ));
        };
        if let Some(want) = expected {
            if want != payload {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `?` has type {}",
                        self.show(want),
                        self.show(payload)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: payload,
            kind: TExprKind::Try {
                expr: Box::new(operand),
                ok_tag,
                fail_tag,
                payload,
                fail,
            },
        })
    }
}
