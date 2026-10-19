//! Comprehensions expand before checking into ordinary, hygienically imported
//! FP operations. The existing checker owns inference, capture and move rules.
use super::*;
mod control;
#[cfg(test)]
mod tests;

const HELPERS: [&str; 4] = ["bind", "map", "flat_map_once", "map_once"];

struct Binding {
    param: Param,
    source: Expr,
    arrow: Span,
    locals: Vec<Stmt>,
}

impl Parser<'_> {
    pub(super) fn add_comprehension_import(&self, imports: &mut Vec<Import>) {
        let names = HELPERS
            .iter()
            .enumerate()
            .filter_map(|(i, name)| {
                self.comprehension_helpers[i].then(|| ImportItem {
                    name: (*name).into(),
                    alias: format!("$for${name}"),
                    span: Span::empty(),
                    alias_span: Span::empty(),
                })
            })
            .collect::<Vec<_>>();
        if !names.is_empty() {
            // Empty spans distinguish compiler dependencies from written imports.
            imports.push(Import {
                span: Span::empty(),
                path_span: Span::empty(),
                path: "std/fp".into(),
                names,
            });
        }
    }

    pub(super) fn parse_comprehension(&mut self, start: Span) -> Result<Expr, CompileError> {
        let once = matches!(&self.current().kind, TokenKind::Ident(n) if n == "once");
        if once {
            self.bump();
        }
        let constructor = if self.eat_lt() {
            let ty = self.parse_type()?;
            self.expect_gt()?;
            Some(ty)
        } else {
            None
        };
        self.expect(&TokenKind::LBrace, "'{' after comprehension")?;
        let mut bindings: Vec<Binding> = Vec::new();
        let result = loop {
            if self.is(&TokenKind::RBrace) || self.is(&TokenKind::Eof) {
                return Err(CompileError::at(
                    self.source,
                    self.current().span.start,
                    "a comprehension needs a final `yield expression;`",
                ));
            }
            if matches!(&self.current().kind, TokenKind::Ident(n) if n == "yield")
                && !self
                    .tokens
                    .get(self.index + 1)
                    .is_some_and(|t| matches!(t.kind, TokenKind::Lt | TokenKind::Colon))
            {
                let at = self.bump().span;
                if bindings.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        at.start,
                        "a comprehension needs a `<-` binding before yield",
                    ));
                }
                let value = self.parse_expr()?;
                control::validate(self.source, &value)?;
                self.eat(&TokenKind::Semi);
                break value;
            }
            if self.is(&TokenKind::Let) {
                if bindings.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        self.current().span.start,
                        "place local `let` declarations after the first comprehension binding",
                    ));
                }
                let local = self.parse_let()?;
                if let Stmt::Let { init, .. } = &local {
                    control::validate(self.source, init)?;
                }
                bindings.last_mut().unwrap().locals.push(local);
                continue;
            }
            if matches!(
                self.current().kind,
                TokenKind::If | TokenKind::Return | TokenKind::Break | TokenKind::Continue
            ) {
                return Err(CompileError::at(self.source, self.current().span.start,
                    "expected a `<-` binding, local `let`, or final `yield`; comprehension guards and nonlocal control flow are not supported"));
            }
            let param = self.parse_param()?;
            let left = self.expect(&TokenKind::Lt, "`<-` after comprehension pattern")?;
            let right = self.expect(&TokenKind::Minus, "`<-` after comprehension pattern")?;
            if left.span.end != right.span.start {
                return Err(CompileError::at(
                    self.source,
                    left.span.start,
                    "write the binding arrow as `<-`",
                ));
            }
            let source = self.parse_expr()?;
            control::validate(self.source, &source)?;
            self.expect(&TokenKind::Semi, "';' after comprehension binding")?;
            bindings.push(Binding {
                param,
                source,
                arrow: left.span.to(right.span),
                locals: Vec::new(),
            });
        };
        let end = self
            .expect(&TokenKind::RBrace, "'}' after final yield")?
            .span;
        let mut expanded = result;
        for (i, binding) in bindings.into_iter().rev().enumerate() {
            let helper = if once { 2 } else { 0 } + usize::from(i == 0);
            self.comprehension_helpers[helper] = true;
            // A binder is not visible inside its own source expression.
            let body_span = Span::new(binding.source.span.end, end.end);
            let call_span = binding.param.name_span.to(end);
            let slot = || ParsedTy::Named {
                span: Span::empty(),
                name: "_".into(),
                args: Vec::new(),
            };
            let type_args = if constructor.is_some() || binding.param.ty.is_some() {
                vec![
                    constructor.clone().unwrap_or_else(slot),
                    binding.param.ty.clone().unwrap_or_else(slot),
                    slot(),
                ]
            } else {
                Vec::new()
            };
            let callback = Expr {
                span: binding.arrow.to(end),
                kind: ExprKind::Lambda {
                    once,
                    params: vec![binding.param],
                    body: Box::new(Expr {
                        span: body_span,
                        kind: ExprKind::Block(Box::new(Block {
                            span: body_span,
                            stmts: binding.locals,
                            tail: Some(expanded),
                        })),
                    }),
                },
            };
            expanded = Expr {
                span: call_span,
                kind: ExprKind::Call {
                    callee: format!("$for${}", HELPERS[helper]),
                    callee_span: Span::new(binding.arrow.start, binding.arrow.start),
                    type_args,
                    args: vec![binding.source, callback],
                },
            };
        }
        let span = start.to(end);
        Ok(Expr {
            span,
            kind: ExprKind::Block(Box::new(Block {
                span,
                stmts: Vec::new(),
                tail: Some(expanded),
            })),
        })
    }
}

pub(super) fn validate_expanded(source: &str, expr: &Expr) -> Result<(), CompileError> {
    control::validate(source, expr)
}
