//! Iteration in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_for(
        &mut self,
        span: usize,
        pat: &Pat,
        iter: &Expr,
        body: &Block,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `for` has type unit"),
                ));
            }
        }
        if let ExprKind::Range {
            start,
            end,
            inclusive,
        } = &iter.kind
        {
            return self.check_for_range(span, pat, start, end, *inclusive, body);
        }
        let iter = self.check_expr(iter, None)?;
        if let Some((nil, cons, elem)) = self.list_shape(iter.ty) {
            return self.check_for_list(pat, iter, nil, cons, elem, body);
        }
        let elem = self.element_of(iter.ty).ok_or_else(|| {
            CompileError::at(
                self.source,
                span,
                format!("cannot iterate over {}", self.show(iter.ty)),
            )
        })?;
        let snap = self.bind(format!("__snap{}", self.bindings.len()), iter.ty, false);
        let index = self.bind(format!("__idx{}", self.bindings.len()), Ty::Usize, true);
        self.scopes.push(HashMap::new());
        let elem_pat = self.check_for_pat(pat, elem, body.span)?;
        if !self.irrefutable_pat(&elem_pat, elem) {
            return Err(CompileError::at(
                self.source,
                pat.span().start,
                "a `for` pattern must be irrefutable",
            ));
        }
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        let loaded = TExpr {
            ty: elem,
            kind: TExprKind::Index {
                base: Box::new(TExpr {
                    ty: iter.ty,
                    kind: TExprKind::Name(snap),
                }),
                index: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Name(index),
                }),
                elem,
            },
        };
        let extract = match elem_pat {
            TPat::Bind(binding) => TStmt::Let {
                binding,
                init: loaded,
            },
            TPat::Wild => TStmt::Expr(loaded),
            other => TStmt::Destructure {
                pat: other,
                init: loaded,
            },
        };
        let iter_expr = TExpr {
            ty: Ty::Unit,
            kind: TExprKind::ForIter {
                index,
                start: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Int(0),
                }),
                end: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Len {
                        vec: Box::new(TExpr {
                            ty: iter.ty,
                            kind: TExprKind::Name(snap),
                        }),
                    },
                }),
                inclusive: false,
                extract: Some(Box::new(extract)),
                body: Box::new(body),
            },
        };
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Block(Box::new(TBlock {
                stmts: vec![TStmt::Let {
                    binding: snap,
                    init: iter,
                }],
                tail: Some(iter_expr),
            })),
        })
    }

    pub(super) fn check_for_range(
        &mut self,
        span: usize,
        pat: &Pat,
        start: &Expr,
        end: &Expr,
        inclusive: bool,
        body: &Block,
    ) -> Result<TExpr, CompileError> {
        let bound = self.common_int(start, end);
        let start = self.check_expr(start, bound)?;
        if !start.ty.is_int() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("a range needs an integer, found {}", self.show(start.ty)),
            ));
        }
        let end_span = end.span.start;
        let end_hint = bound
            .or_else(|| self.peek(end).filter(|ty| ty.is_int()))
            .or(Some(start.ty));
        let end = self.check_expr(end, end_hint)?;
        if end.ty != start.ty {
            return Err(CompileError::at(
                self.source,
                end_span,
                format!(
                    "a range's bounds have types {} and {}",
                    self.show(start.ty),
                    self.show(end.ty)
                ),
            ));
        }
        self.scopes.push(HashMap::new());
        let bound = self.check_for_pat(pat, start.ty, body.span)?;
        let index = match bound {
            TPat::Bind(id) => id,
            TPat::Wild => self.bind(format!("__i{}", self.bindings.len()), start.ty, true),
            _ => {
                return Err(CompileError::at(
                    self.source,
                    pat.span().start,
                    "a range binds one name",
                ));
            }
        };
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::ForIter {
                index,
                start: Box::new(start),
                end: Box::new(end),
                inclusive,
                extract: None,
                body: Box::new(body),
            },
        })
    }

    /// An unsuffixed literal adopts the other bound. `0..text.len()` is `usize`.
    /// Two unsuffixed literals use the narrowest signed type that holds both.
    pub(super) fn common_int(&self, start: &Expr, end: &Expr) -> Option<Ty> {
        let left = self.peek(start).filter(|ty| ty.is_int());
        let right = self.peek(end).filter(|ty| ty.is_int());
        match (left, right) {
            (Some(left), Some(right)) if left == right => Some(left),
            (Some(left), None) => Some(left),
            (None, Some(right)) => Some(right),
            (None, None) => match (literal_int(start), literal_int(end)) {
                (Some(start), Some(end)) => Some(signed_fit(start, end)),
                _ => None,
            },
            _ => None,
        }
    }

    pub(super) fn list_shape(&self, ty: Ty) -> Option<(u16, u16, Ty)> {
        let Ty::Adt(id) = ty else {
            return None;
        };
        let adt = &self.adts[id as usize];
        if self.schemas[adt.schema].name != "List" {
            return None;
        }
        let mut nil = None;
        let mut cons = None;
        let mut elem = None;
        for (tag, ctor) in adt.ctors.iter().enumerate() {
            if ctor.fields.is_empty() {
                nil = Some(tag as u16);
            }
            if ctor.fields.len() == 2 && ctor.fields[1] == ty {
                cons = Some(tag as u16);
                elem = Some(ctor.fields[0]);
            }
        }
        Some((nil?, cons?, elem?))
    }

    pub(super) fn check_for_list(
        &mut self,
        pat: &Pat,
        iter: TExpr,
        nil: u16,
        cons: u16,
        elem: Ty,
        body: &Block,
    ) -> Result<TExpr, CompileError> {
        let list_ty = iter.ty;
        let cursor = self.bind(format!("__cur{}", self.bindings.len()), list_ty, true);
        self.scopes.push(HashMap::new());
        let elem_pat = self.check_for_pat(pat, elem, body.span)?;
        if !self.irrefutable_pat(&elem_pat, elem) {
            return Err(CompileError::at(
                self.source,
                pat.span().start,
                "a `for` pattern must be irrefutable",
            ));
        }
        let rest = self.bind(format!("__rest{}", self.bindings.len()), list_ty, false);
        self.loops += 1;
        let user = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        let mut stmts = vec![TStmt::Assign {
            binding: cursor,
            value: TExpr {
                ty: list_ty,
                kind: TExprKind::Name(rest),
            },
        }];
        stmts.extend(user.stmts);
        if let Some(tail) = user.tail {
            stmts.push(TStmt::Expr(tail));
        }
        let matched = TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Match {
                scrutinee: Box::new(TExpr {
                    ty: list_ty,
                    kind: TExprKind::Name(cursor),
                }),
                arms: vec![
                    TArm {
                        pat: TPat::Ctor {
                            tag: nil,
                            fields: Vec::new(),
                        },
                        guard: None,
                        body: TBlock {
                            stmts: vec![TStmt::Break],
                            tail: None,
                        },
                    },
                    TArm {
                        pat: TPat::Ctor {
                            tag: cons,
                            fields: vec![
                                FieldPat {
                                    ty: elem,
                                    pat: Box::new(elem_pat),
                                },
                                FieldPat {
                                    ty: list_ty,
                                    pat: Box::new(TPat::Bind(rest)),
                                },
                            ],
                        },
                        guard: None,
                        body: TBlock { stmts, tail: None },
                    },
                ],
            },
        };
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Block(Box::new(TBlock {
                stmts: vec![TStmt::Let {
                    binding: cursor,
                    init: iter,
                }],
                tail: Some(TExpr {
                    ty: Ty::Unit,
                    kind: TExprKind::While {
                        cond: Box::new(TExpr {
                            ty: Ty::Bool,
                            kind: TExprKind::Bool(true),
                        }),
                        body: Box::new(TBlock {
                            stmts: Vec::new(),
                            tail: Some(matched),
                        }),
                    },
                }),
            })),
        })
    }

    fn check_for_pat(&mut self, pat: &Pat, ty: Ty, scope: Span) -> Result<TPat, CompileError> {
        self.scope_spans.push(scope);
        let result = self.check_pat(pat, ty, false);
        self.scope_spans.pop();
        result
    }
}
