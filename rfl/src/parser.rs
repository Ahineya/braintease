use crate::ast::*;
use crate::error::CompileError;
use crate::lexer::{Token, TokenKind};

pub fn parse(source: &str, tokens: &[Token]) -> Result<Module, CompileError> {
    let mut parser = Parser { source, tokens, index: 0 };
    let mut adts = Vec::new();
    let mut structs = Vec::new();
    let mut impls = Vec::new();
    let mut consts = Vec::new();
    let mut functions = Vec::new();
    while !parser.is(&TokenKind::Eof) {
        if parser.is(&TokenKind::Type) {
            adts.push(parser.parse_adt()?);
        } else if parser.is(&TokenKind::Struct) {
            structs.push(parser.parse_struct()?);
        } else if parser.is(&TokenKind::Impl) {
            impls.push(parser.parse_impl()?);
        } else if parser.is(&TokenKind::Const) {
            consts.push(parser.parse_const()?);
        } else if parser.is(&TokenKind::Fn) {
            functions.push(parser.parse_function()?);
        } else {
            let token = parser.bump();
            return Err(CompileError::at(source, token.span.start, "expected `fn`, `type`, `struct`, `impl`, or `const`"));
        }
    }
    if functions.is_empty() {
        return Err(CompileError::at(source, 0, "expected a function"));
    }
    Ok(Module { adts, structs, impls, consts, functions })
}

struct Parser<'a> {
    source: &'a str,
    tokens: &'a [Token],
    index: usize,
}

impl<'a> Parser<'a> {
    fn parse_function(&mut self) -> Result<Function, CompileError> {
        self.expect(&TokenKind::Fn, "fn")?;
        let name_tok = self.expect_ident()?;
        let type_params = self.parse_generic_params()?;
        self.expect(&TokenKind::LParen, "'('")?;
        let mut params = Vec::new();
        if !self.is(&TokenKind::RParen) {
            loop {
                params.push(self.parse_param()?);
                if self.eat(&TokenKind::Comma) {
                    continue;
                }
                break;
            }
        }
        self.expect(&TokenKind::RParen, "')'")?;
        let ret = if self.eat(&TokenKind::Arrow) {
            Some(self.parse_type()?)
        } else {
            None
        };
        let body = self.parse_block()?;
        Ok(Function {
            name: name_tok.0,
            name_span: name_tok.1,
            type_params,
            params,
            ret,
            body,
        })
    }

    fn parse_impl(&mut self) -> Result<Impl, CompileError> {
        let span = self.expect(&TokenKind::Impl, "impl")?.span;
        let type_params = self.parse_generic_params()?;
        let ty = self.parse_type()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut items = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            self.eat(&TokenKind::Export);
            items.push(self.parse_method()?);
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Impl { span, type_params, ty, items })
    }

    fn parse_method(&mut self) -> Result<ImplItem, CompileError> {
        self.expect(&TokenKind::Fn, "fn")?;
        let (name, name_span) = self.expect_ident()?;
        let type_params = self.parse_generic_params()?;
        self.expect(&TokenKind::LParen, "'('")?;
        let mut receiver = None;
        let mut params = Vec::new();
        if !self.is(&TokenKind::RParen) {
            if self.starts_receiver() {
                receiver = Some(self.parse_receiver()?);
                if self.eat(&TokenKind::Comma) && !self.is(&TokenKind::RParen) {
                    loop {
                        params.push(self.parse_param()?);
                        if self.eat(&TokenKind::Comma) {
                            continue;
                        }
                        break;
                    }
                }
            } else {
                loop {
                    params.push(self.parse_param()?);
                    if self.eat(&TokenKind::Comma) {
                        continue;
                    }
                    break;
                }
            }
        }
        self.expect(&TokenKind::RParen, "')'")?;
        let ret = if self.eat(&TokenKind::Arrow) { Some(self.parse_type()?) } else { None };
        let body = self.parse_block()?;
        Ok(ImplItem { name, name_span, type_params, receiver, params, ret, body })
    }

    fn starts_receiver(&self) -> bool {
        match &self.current().kind {
            TokenKind::Ident(name) if name == "self" => true,
            TokenKind::Amp => match self.tokens.get(self.index + 1).map(|token| &token.kind) {
                Some(TokenKind::Ident(name)) if name == "self" => true,
                Some(TokenKind::Mut) => {
                    matches!(self.tokens.get(self.index + 2).map(|token| &token.kind), Some(TokenKind::Ident(name)) if name == "self")
                }
                _ => false,
            },
            _ => false,
        }
    }

    fn parse_receiver(&mut self) -> Result<Recv, CompileError> {
        if self.eat(&TokenKind::Amp) {
            let mutable = self.eat(&TokenKind::Mut);
            let (name, span) = self.expect_ident()?;
            if name != "self" {
                return Err(CompileError::at(self.source, span.start, "expected `self`"));
            }
            return Ok(if mutable { Recv::Mut } else { Recv::Shared });
        }
        let (name, span) = self.expect_ident()?;
        if name != "self" {
            return Err(CompileError::at(self.source, span.start, "expected `self`"));
        }
        Ok(Recv::Value)
    }

    fn parse_adt(&mut self) -> Result<Adt, CompileError> {
        self.expect(&TokenKind::Type, "type")?;
        let (name, name_span) = self.expect_ident()?;
        let params = self.parse_generic_params()?;
        self.expect(&TokenKind::Eq, "'='")?;
        self.eat(&TokenKind::Pipe);
        let mut ctors = Vec::new();
        loop {
            ctors.push(self.parse_ctor()?);
            if self.eat(&TokenKind::Pipe) {
                continue;
            }
            break;
        }
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Adt { name, name_span, params, ctors })
    }

    fn parse_generic_params(&mut self) -> Result<Vec<TypeParam>, CompileError> {
        if !self.eat(&TokenKind::Lt) {
            return Ok(Vec::new());
        }
        let mut params = Vec::new();
        loop {
            let (name, span) = self.expect_ident()?;
            params.push(TypeParam { name, span });
            if self.eat(&TokenKind::Comma) {
                continue;
            }
            break;
        }
        self.expect(&TokenKind::Gt, "'>'")?;
        Ok(params)
    }

    /// `name<ty, ...>(...)`. Anything else, including `a < b`, is left for the
    /// comparison parser.
    fn try_generic_call(&mut self, callee: String, span: Span) -> Result<Option<Expr>, CompileError> {
        if !self.is(&TokenKind::Lt) {
            return Ok(None);
        }
        let saved = self.index;
        self.bump();
        if !self.is_ident() {
            self.index = saved;
            return Ok(None);
        }
        let mut type_args = Vec::new();
        loop {
            match self.parse_type() {
                Ok(ty) => type_args.push(ty),
                Err(_) => {
                    self.index = saved;
                    return Ok(None);
                }
            }
            if self.eat(&TokenKind::Comma) {
                if !self.is_ident() {
                    self.index = saved;
                    return Ok(None);
                }
                continue;
            }
            break;
        }
        if !self.eat(&TokenKind::Gt) || !self.is(&TokenKind::LParen) {
            self.index = saved;
            return Ok(None);
        }
        self.bump();
        let mut args = Vec::new();
        if !self.is(&TokenKind::RParen) {
            loop {
                args.push(self.parse_expr()?);
                if self.eat(&TokenKind::Comma) {
                    continue;
                }
                break;
            }
        }
        self.expect(&TokenKind::RParen, "')'")?;
        Ok(Some(Expr { span, kind: ExprKind::Call { callee, type_args, args } }))
    }

    fn parse_ctor(&mut self) -> Result<Ctor, CompileError> {
        let (name, name_span) = self.expect_ident()?;
        let mut fields = Vec::new();
        if self.eat(&TokenKind::LParen) {
            if !self.is(&TokenKind::RParen) {
                loop {
                    fields.push(self.parse_type()?);
                    if self.eat(&TokenKind::Comma) {
                        continue;
                    }
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
        }
        Ok(Ctor { name, name_span, fields })
    }

    fn parse_const(&mut self) -> Result<Const, CompileError> {
        self.expect(&TokenKind::Const, "const")?;
        let (name, name_span) = self.expect_ident()?;
        self.expect(&TokenKind::Colon, "':'")?;
        let ty = self.parse_type()?;
        self.expect(&TokenKind::Eq, "'='")?;
        let init = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Const { name, name_span, ty, init })
    }

    fn parse_struct(&mut self) -> Result<StructItem, CompileError> {
        self.expect(&TokenKind::Struct, "struct")?;
        let (name, name_span) = self.expect_ident()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut fields = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let (field_name, field_span) = self.expect_ident()?;
            self.expect(&TokenKind::Colon, "':'")?;
            let ty = self.parse_type()?;
            fields.push(StructField { name: field_name, name_span: field_span, ty });
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        if fields.is_empty() {
            return Err(CompileError::at(self.source, name_span.start, "a struct needs a field"));
        }
        Ok(StructItem { name, name_span, fields })
    }

    fn parse_param(&mut self) -> Result<Param, CompileError> {
        let pat = self.parse_pat()?;
        let (name, name_span) = match &pat {
            Pat::Name { name, span } if name != "_" => (name.clone(), *span),
            _ => (String::new(), pat.span()),
        };
        let ty = if self.eat(&TokenKind::Colon) { Some(self.parse_type()?) } else { None };
        Ok(Param { name, name_span, ty, pat })
    }

    fn parse_type(&mut self) -> Result<ParsedTy, CompileError> {
        if self.eat(&TokenKind::LParen) {
            let span = self.prev_span();
            if self.eat(&TokenKind::RParen) {
                return Ok(ParsedTy::Named { span, name: "unit".to_string(), args: Vec::new() });
            }
            let first = self.parse_type()?;
            if self.eat(&TokenKind::Comma) {
                let mut fields = vec![first];
                if !self.is(&TokenKind::RParen) {
                    loop {
                        fields.push(self.parse_type()?);
                        if self.eat(&TokenKind::Comma) {
                            if self.is(&TokenKind::RParen) {
                                break;
                            }
                            continue;
                        }
                        break;
                    }
                }
                self.expect(&TokenKind::RParen, "')'")?;
                return Ok(ParsedTy::Tuple { span, fields });
            }
            self.expect(&TokenKind::RParen, "')'")?;
            return Ok(first);
        }
        if self.eat(&TokenKind::LBracket) {
            let span = self.prev_span();
            let elem = self.parse_type()?;
            self.expect(&TokenKind::Semi, "';'")?;
            let len = self.parse_expr()?;
            self.expect(&TokenKind::RBracket, "']'")?;
            return Ok(ParsedTy::Array { span, elem: Box::new(elem), len: Box::new(len) });
        }
        if self.eat(&TokenKind::Amp) {
            let span = self.prev_span();
            let mutable = self.eat(&TokenKind::Mut);
            let inner = self.parse_type()?;
            return Ok(ParsedTy::Ref { span, mutable, inner: Box::new(inner) });
        }
        if self.eat(&TokenKind::Fn) {
            let span = self.prev_span();
            self.expect(&TokenKind::LParen, "'('")?;
            let mut params = Vec::new();
            if !self.is(&TokenKind::RParen) {
                loop {
                    params.push(self.parse_type()?);
                    if self.eat(&TokenKind::Comma) {
                        continue;
                    }
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
            let ret = if self.eat(&TokenKind::Arrow) {
                self.parse_type()?
            } else {
                ParsedTy::Named { span, name: "unit".to_string(), args: Vec::new() }
            };
            return Ok(ParsedTy::Func { span, params, ret: Box::new(ret) });
        }
        let (name, span) = self.expect_ident()?;
        let mut args = Vec::new();
        if self.eat(&TokenKind::Lt) {
            loop {
                args.push(self.parse_type()?);
                if self.eat(&TokenKind::Comma) {
                    continue;
                }
                break;
            }
            self.expect(&TokenKind::Gt, "'>'")?;
        }
        Ok(ParsedTy::Named { name, span, args })
    }

    fn parse_lambda(&mut self, span: Span) -> Result<Expr, CompileError> {
        if self.is(&TokenKind::Lt) {
            return Err(CompileError::at(self.source, span.start, "generic lambdas are not implemented yet"));
        }
        self.expect(&TokenKind::LParen, "'('")?;
        let mut params = Vec::new();
        if !self.is(&TokenKind::RParen) {
            loop {
                params.push(self.parse_param()?);
                if self.eat(&TokenKind::Comma) {
                    continue;
                }
                break;
            }
        }
        self.expect(&TokenKind::RParen, "')'")?;
        let body = if self.eat(&TokenKind::FatArrow) {
            self.parse_expr()?
        } else {
            let block = self.parse_block()?;
            Expr { span, kind: ExprKind::Block(Box::new(block)) }
        };
        Ok(Expr { span, kind: ExprKind::Lambda { params, body: Box::new(body) } })
    }

    fn parse_block(&mut self) -> Result<Block, CompileError> {
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut stmts = Vec::new();
        let mut tail = None;
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            if self.is(&TokenKind::Let) {
                stmts.push(self.parse_let()?);
                continue;
            }
            if self.is(&TokenKind::Return) {
                stmts.push(self.parse_return()?);
                continue;
            }
            if self.is(&TokenKind::Break) {
                stmts.push(self.parse_break()?);
                continue;
            }
            if self.is(&TokenKind::Continue) {
                stmts.push(self.parse_continue()?);
                continue;
            }
            if self.is_ident() && self.peek_assign() {
                stmts.push(self.parse_assign()?);
                continue;
            }
            let expr = self.parse_expr()?;
            if self.is(&TokenKind::Eq) {
                stmts.push(self.finish_place_assign(expr)?);
                continue;
            }
            if self.eat(&TokenKind::Semi) {
                stmts.push(Stmt::Expr(expr));
                continue;
            }
            if expr.ends_with_block() && !self.is(&TokenKind::RBrace) {
                stmts.push(Stmt::Expr(expr));
                continue;
            }
            tail = Some(expr);
            break;
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Block { stmts, tail })
    }

    fn parse_let(&mut self) -> Result<Stmt, CompileError> {
        self.expect(&TokenKind::Let, "let")?;
        let mutable = self.eat(&TokenKind::Mut);
        let pattern = self.parse_pat()?;
        let ty = if self.eat(&TokenKind::Colon) { Some(self.parse_type()?) } else { None };
        self.expect(&TokenKind::Eq, "'='")?;
        let init = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::Let { pattern, mutable, ty, init })
    }

    fn parse_assign(&mut self) -> Result<Stmt, CompileError> {
        let (name, name_span) = self.expect_ident()?;
        let op = self.compound_op()?;
        let rhs = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        let value = match op {
            None => rhs,
            Some(op) => {
                let lhs = Expr { span: name_span, kind: ExprKind::Name(name.clone()) };
                self.binary(op, lhs, rhs)
            }
        };
        Ok(Stmt::Assign { name, name_span, value })
    }

    fn finish_place_assign(&mut self, expr: Expr) -> Result<Stmt, CompileError> {
        if let ExprKind::Field { .. } = &expr.kind {
            let mut fields = Vec::new();
            let mut cursor = expr;
            let root = loop {
                match cursor.kind {
                    ExprKind::Field { base, name } => {
                        fields.push(name);
                        cursor = *base;
                    }
                    ExprKind::Name(name) => break (name, cursor.span),
                    _ => {
                        return Err(CompileError::at(self.source, cursor.span.start, "field assignment needs a name"));
                    }
                }
            };
            fields.reverse();
            self.expect(&TokenKind::Eq, "'='")?;
            let value = self.parse_expr()?;
            self.expect(&TokenKind::Semi, "';'")?;
            return Ok(Stmt::FieldAssign { name: root.0, name_span: root.1, fields, value });
        }
        self.finish_index_assign(expr)
    }

    fn finish_index_assign(&mut self, expr: Expr) -> Result<Stmt, CompileError> {
        let ExprKind::Index { base, index } = expr.kind else {
            return Err(CompileError::at(self.source, expr.span.start, "expected an index before '='"));
        };
        let ExprKind::Name(name) = base.kind else {
            return Err(CompileError::at(self.source, base.span.start, "index assignment needs a name"));
        };
        self.expect(&TokenKind::Eq, "'='")?;
        let value = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::IndexAssign { name, name_span: base.span, index: *index, value })
    }

    fn compound_op(&mut self) -> Result<Option<BinOp>, CompileError> {
        let token = self.bump();
        let op = match token.kind {
            TokenKind::Eq => None,
            TokenKind::PlusEq => Some(BinOp::Add),
            TokenKind::MinusEq => Some(BinOp::Sub),
            TokenKind::StarEq => Some(BinOp::Mul),
            TokenKind::SlashEq => Some(BinOp::Div),
            TokenKind::PercentEq => Some(BinOp::Rem),
            _ => return Err(CompileError::at(self.source, token.span.start, "expected '='")),
        };
        Ok(op)
    }

    fn parse_break(&mut self) -> Result<Stmt, CompileError> {
        let span = self.expect(&TokenKind::Break, "break")?.span;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::Break { span })
    }

    fn parse_continue(&mut self) -> Result<Stmt, CompileError> {
        let span = self.expect(&TokenKind::Continue, "continue")?.span;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::Continue { span })
    }

    fn parse_return(&mut self) -> Result<Stmt, CompileError> {
        let span = self.expect(&TokenKind::Return, "return")?.span;
        let value = if self.is(&TokenKind::Semi) {
            None
        } else {
            Some(self.parse_expr()?)
        };
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::Return { span, value })
    }

    fn parse_expr(&mut self) -> Result<Expr, CompileError> {
        self.parse_or()
    }

    fn parse_or(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_and()?;
        while self.eat(&TokenKind::OrOr) {
            let rhs = self.parse_and()?;
            expr = self.binary(BinOp::Or, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_and(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_equality()?;
        while self.eat(&TokenKind::AndAnd) {
            let rhs = self.parse_equality()?;
            expr = self.binary(BinOp::And, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_equality(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_compare()?;
        loop {
            let op = if self.eat(&TokenKind::EqEq) {
                BinOp::Eq
            } else if self.eat(&TokenKind::NotEq) {
                BinOp::Ne
            } else {
                break;
            };
            let rhs = self.parse_compare()?;
            expr = self.binary(op, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_compare(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_range()?;
        loop {
            let op = if self.eat(&TokenKind::Lt) {
                BinOp::Lt
            } else if self.eat(&TokenKind::Le) {
                BinOp::Le
            } else if self.eat(&TokenKind::Gt) {
                BinOp::Gt
            } else if self.eat(&TokenKind::Ge) {
                BinOp::Ge
            } else {
                break;
            };
            let rhs = self.parse_term()?;
            expr = self.binary(op, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_range(&mut self) -> Result<Expr, CompileError> {
        let expr = self.parse_term()?;
        let inclusive = self.eat(&TokenKind::DotDotEq);
        if inclusive || self.eat(&TokenKind::DotDot) {
            let end = self.parse_term()?;
            return Ok(Expr {
                span: expr.span,
                kind: ExprKind::Range { start: Box::new(expr), end: Box::new(end), inclusive },
            });
        }
        Ok(expr)
    }

    fn parse_term(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_factor()?;
        loop {
            let op = if self.eat(&TokenKind::Plus) {
                BinOp::Add
            } else if self.eat(&TokenKind::Minus) {
                BinOp::Sub
            } else {
                break;
            };
            let rhs = self.parse_factor()?;
            expr = self.binary(op, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_factor(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_unary()?;
        loop {
            let op = if self.eat(&TokenKind::Star) {
                BinOp::Mul
            } else if self.eat(&TokenKind::Slash) {
                BinOp::Div
            } else if self.eat(&TokenKind::Percent) {
                BinOp::Rem
            } else {
                break;
            };
            let rhs = self.parse_unary()?;
            expr = self.binary(op, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_unary(&mut self) -> Result<Expr, CompileError> {
        if self.eat(&TokenKind::Amp) {
            let span = self.prev_span();
            let mutable = self.eat(&TokenKind::Mut);
            let expr = self.parse_unary()?;
            return Ok(Expr { span, kind: ExprKind::Borrow { mutable, expr: Box::new(expr) } });
        }
        if self.eat(&TokenKind::Minus) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span,
                kind: ExprKind::Unary { op: UnaryOp::Neg, expr: Box::new(expr) },
            });
        }
        if self.eat(&TokenKind::Not) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span,
                kind: ExprKind::Unary { op: UnaryOp::Not, expr: Box::new(expr) },
            });
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_primary()?;
        loop {
            if let ExprKind::Name(name) = expr.kind.clone() {
                if let Some(call) = self.try_generic_call(name, expr.span)? {
                    expr = call;
                    continue;
                }
            }
            if let ExprKind::Name(ty_name) = expr.kind.clone() {
                if self.is(&TokenKind::LBrace) && self.looks_like_struct_body() {
                    expr = self.parse_struct_lit(ty_name, expr.span)?;
                    continue;
                }
                if self.eat(&TokenKind::ColonColon) {
                    let (method, span) = self.expect_ident()?;
                    self.expect(&TokenKind::LParen, "'('")?;
                    let mut args = Vec::new();
                    if !self.is(&TokenKind::RParen) {
                        loop {
                            args.push(self.parse_expr()?);
                            if self.eat(&TokenKind::Comma) {
                                continue;
                            }
                            break;
                        }
                    }
                    self.expect(&TokenKind::RParen, "')'")?;
                    expr = Expr { span, kind: ExprKind::Assoc { ty_name, method, args } };
                    continue;
                }
            }
            if self.eat(&TokenKind::LParen) {
                let mut args = Vec::new();
                if !self.is(&TokenKind::RParen) {
                    loop {
                        args.push(self.parse_expr()?);
                        if self.eat(&TokenKind::Comma) {
                            continue;
                        }
                        break;
                    }
                }
                self.expect(&TokenKind::RParen, "')'")?;
                let callee = match expr.kind {
                    ExprKind::Name(name) => name,
                    _ => {
                        return Err(CompileError::at(
                            self.source,
                            expr.span.start,
                            "only a function name can be called",
                        ));
                    }
                };
                expr = Expr { span: expr.span, kind: ExprKind::Call { callee, type_args: Vec::new(), args } };
            } else if self.eat(&TokenKind::LBracket) {
                let inside = self.parse_expr()?;
                if let ExprKind::Range { start, end, inclusive } = inside.kind {
                    self.expect(&TokenKind::RBracket, "']'")?;
                    expr = Expr {
                        span: expr.span,
                        kind: ExprKind::Slice { base: Box::new(expr), start, end, inclusive },
                    };
                } else {
                    let inclusive = self.eat(&TokenKind::DotDotEq);
                    if inclusive || self.eat(&TokenKind::DotDot) {
                        let end = self.parse_expr()?;
                        self.expect(&TokenKind::RBracket, "']'")?;
                        expr = Expr {
                            span: expr.span,
                            kind: ExprKind::Slice {
                                base: Box::new(expr),
                                start: Box::new(inside),
                                end: Box::new(end),
                                inclusive,
                            },
                        };
                    } else {
                        self.expect(&TokenKind::RBracket, "']'")?;
                        expr = Expr {
                            span: expr.span,
                            kind: ExprKind::Index { base: Box::new(expr), index: Box::new(inside) },
                        };
                    }
                }
            } else if self.eat(&TokenKind::Dot) {
                let (method, span) = self.expect_ident()?;
                if !self.is(&TokenKind::LParen) {
                    expr = Expr {
                        span,
                        kind: ExprKind::Field { base: Box::new(expr), name: method },
                    };
                    continue;
                }
                self.expect(&TokenKind::LParen, "'('")?;
                let mut args = Vec::new();
                if !self.is(&TokenKind::RParen) {
                    loop {
                        args.push(self.parse_expr()?);
                        if self.eat(&TokenKind::Comma) {
                            continue;
                        }
                        break;
                    }
                }
                self.expect(&TokenKind::RParen, "')'")?;
                expr = Expr { span, kind: ExprKind::Method { receiver: Box::new(expr), method, args } };
            } else if self.eat(&TokenKind::Question) {
                expr = Expr { span: expr.span, kind: ExprKind::Try(Box::new(expr)) };
            } else if self.eat(&TokenKind::As) {
                let ty = self.parse_type()?;
                expr = Expr {
                    span: expr.span,
                    kind: ExprKind::Cast { expr: Box::new(expr), ty },
                };
            } else {
                break;
            }
        }
        Ok(expr)
    }

    fn parse_primary(&mut self) -> Result<Expr, CompileError> {
        let token = self.bump();
        match token.kind.clone() {
            TokenKind::Int(value) => Ok(Expr { span: token.span, kind: ExprKind::Int(value) }),
            TokenKind::Str(chars) => Ok(Expr { span: token.span, kind: ExprKind::Str(chars) }),
            TokenKind::True => Ok(Expr { span: token.span, kind: ExprKind::Bool(true) }),
            TokenKind::False => Ok(Expr { span: token.span, kind: ExprKind::Bool(false) }),
            TokenKind::Ident(name) => Ok(Expr { span: token.span, kind: ExprKind::Name(name) }),
            TokenKind::Fn => self.parse_lambda(token.span),
            TokenKind::If => self.parse_if(token.span),
            TokenKind::While => self.parse_while(token.span),
            TokenKind::Match => self.parse_match(token.span),
            TokenKind::For => self.parse_for(token.span),
            TokenKind::LParen => self.parse_paren(token.span),
            TokenKind::LBracket => self.parse_array(token.span),
            TokenKind::LBrace => {
                self.index -= 1;
                let block = self.parse_block()?;
                Ok(Expr { span: token.span, kind: ExprKind::Block(Box::new(block)) })
            }
            other => Err(self.unexpected(&token, &other)),
        }
    }

    fn parse_array(&mut self, span: Span) -> Result<Expr, CompileError> {
        let mut elems = Vec::new();
        if !self.is(&TokenKind::RBracket) {
            loop {
                elems.push(self.parse_expr()?);
                if self.eat(&TokenKind::Comma) {
                    if self.is(&TokenKind::RBracket) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        self.expect(&TokenKind::RBracket, "']'")?;
        Ok(Expr { span, kind: ExprKind::Array(elems) })
    }

    fn parse_if(&mut self, span: Span) -> Result<Expr, CompileError> {
        self.expect(&TokenKind::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect(&TokenKind::RParen, "')'")?;
        let then_body = self.parse_block()?;
        let else_body = if self.eat(&TokenKind::Else) {
            if self.is(&TokenKind::If) {
                let else_span = self.bump().span;
                Some(Box::new(self.parse_if(else_span)?))
            } else {
                let block = self.parse_block()?;
                Some(Box::new(Expr { span, kind: ExprKind::Block(Box::new(block)) }))
            }
        } else {
            None
        };
        Ok(Expr {
            span,
            kind: ExprKind::If { cond: Box::new(cond), then_body: Box::new(then_body), else_body },
        })
    }

    fn parse_match(&mut self, span: Span) -> Result<Expr, CompileError> {
        let scrutinee = self.parse_expr()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut arms = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let pat = self.parse_pat()?;
            let guard = if self.eat(&TokenKind::If) { Some(self.parse_expr()?) } else { None };
            self.expect(&TokenKind::FatArrow, "'=>'")?;
            let body = self.parse_expr()?;
            self.eat(&TokenKind::Comma);
            arms.push(Arm { pat, guard, body });
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        if arms.is_empty() {
            return Err(CompileError::at(self.source, span.start, "match has no arms"));
        }
        Ok(Expr {
            span,
            kind: ExprKind::Match { scrutinee: Box::new(scrutinee), arms },
        })
    }

    /// `{ field` followed by `:`, `,`, or `}` is a struct literal. A match arm is `{ Name =>`.
    fn looks_like_struct_body(&self) -> bool {
        let Some(TokenKind::Ident(_)) = self.tokens.get(self.index + 1).map(|token| &token.kind) else {
            return false;
        };
        matches!(
            self.tokens.get(self.index + 2).map(|token| &token.kind),
            Some(TokenKind::Colon | TokenKind::Comma | TokenKind::RBrace)
        )
    }

    fn parse_struct_lit(&mut self, name: String, span: Span) -> Result<Expr, CompileError> {
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut fields = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let (field, _) = self.expect_ident()?;
            let value = if self.eat(&TokenKind::Colon) {
                self.parse_expr()?
            } else {
                Expr { span, kind: ExprKind::Name(field.clone()) }
            };
            fields.push((field, value));
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Expr { span, kind: ExprKind::StructLit { name, fields } })
    }

    fn parse_paren(&mut self, span: Span) -> Result<Expr, CompileError> {
        if self.eat(&TokenKind::RParen) {
            return Err(CompileError::at(self.source, span.start, "expected an expression"));
        }
        let first = self.parse_expr()?;
        if self.eat(&TokenKind::Comma) {
            let mut fields = vec![first];
            if !self.is(&TokenKind::RParen) {
                loop {
                    fields.push(self.parse_expr()?);
                    if self.eat(&TokenKind::Comma) {
                        if self.is(&TokenKind::RParen) {
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
            return Ok(Expr { span, kind: ExprKind::Tuple(fields) });
        }
        self.expect(&TokenKind::RParen, "')'")?;
        Ok(first)
    }

    fn parse_for(&mut self, span: Span) -> Result<Expr, CompileError> {
        self.expect(&TokenKind::LParen, "'('")?;
        let pat = self.parse_pat()?;
        self.expect(&TokenKind::In, "`in`")?;
        let iter = self.parse_expr()?;
        self.expect(&TokenKind::RParen, "')'")?;
        let body = self.parse_block()?;
        Ok(Expr { span, kind: ExprKind::For { pat, iter: Box::new(iter), body: Box::new(body) } })
    }

    fn parse_pat(&mut self) -> Result<Pat, CompileError> {
        if self.is(&TokenKind::LParen) {
            let span = self.bump().span;
            return self.parse_tuple_pat(span);
        }
        if let TokenKind::Int(value) = &self.tokens[self.index].kind {
            let value = *value;
            let span = self.bump().span;
            return Ok(Pat::Int { value, span });
        }
        if self.is(&TokenKind::True) || self.is(&TokenKind::False) {
            let token = self.bump();
            return Ok(Pat::Bool { value: token.kind == TokenKind::True, span: token.span });
        }
        let (name, span) = self.expect_ident()?;
        if self.eat(&TokenKind::LBrace) {
            return self.parse_struct_pat(name, span);
        }
        if self.eat(&TokenKind::LParen) {
            let mut fields = Vec::new();
            if !self.is(&TokenKind::RParen) {
                loop {
                    fields.push(self.parse_pat()?);
                    if self.eat(&TokenKind::Comma) {
                        continue;
                    }
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
            Ok(Pat::Ctor { name, span, fields })
        } else {
            Ok(Pat::Name { name, span })
        }
    }

    fn parse_tuple_pat(&mut self, span: Span) -> Result<Pat, CompileError> {
        if self.eat(&TokenKind::RParen) {
            return Err(CompileError::at(self.source, span.start, "expected a pattern"));
        }
        let first = self.parse_pat()?;
        if self.eat(&TokenKind::Comma) {
            let mut fields = vec![first];
            if !self.is(&TokenKind::RParen) {
                loop {
                    fields.push(self.parse_pat()?);
                    if self.eat(&TokenKind::Comma) {
                        if self.is(&TokenKind::RParen) {
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            self.expect(&TokenKind::RParen, "')'")?;
            return Ok(Pat::Tuple { span, fields });
        }
        self.expect(&TokenKind::RParen, "')'")?;
        Ok(first)
    }

    fn parse_struct_pat(&mut self, name: String, span: Span) -> Result<Pat, CompileError> {
        let mut fields = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let (field, field_span) = self.expect_ident()?;
            let pat = if self.eat(&TokenKind::Colon) {
                self.parse_pat()?
            } else {
                Pat::Name { name: field.clone(), span: field_span }
            };
            fields.push((field, field_span, pat));
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Pat::Struct { name, span, fields })
    }

    fn parse_while(&mut self, span: Span) -> Result<Expr, CompileError> {
        self.expect(&TokenKind::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect(&TokenKind::RParen, "')'")?;
        let body = self.parse_block()?;
        Ok(Expr {
            span,
            kind: ExprKind::While { cond: Box::new(cond), body: Box::new(body) },
        })
    }

    fn binary(&self, op: BinOp, lhs: Expr, rhs: Expr) -> Expr {
        Expr {
            span: lhs.span,
            kind: ExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) },
        }
    }

    fn unexpected(&self, token: &Token, kind: &TokenKind) -> CompileError {
        let message = match kind {
            TokenKind::Struct => "unexpected `struct`",
            TokenKind::Export | TokenKind::Import => "modules are not implemented yet",
            TokenKind::Extern => "extern blocks are not implemented yet",
            TokenKind::Unsafe => "unsafe is not implemented yet",
            TokenKind::Eof => "unexpected end of file",
            _ => "unexpected token",
        };
        CompileError::at(self.source, token.span.start, message)
    }

    fn expect_ident(&mut self) -> Result<(String, Span), CompileError> {
        let token = self.bump();
        match token.kind {
            TokenKind::Ident(name) => Ok((name, token.span)),
            _ => Err(CompileError::at(self.source, token.span.start, "expected a name")),
        }
    }

    fn expect(&mut self, kind: &TokenKind, what: &str) -> Result<Token, CompileError> {
        let token = self.bump();
        if &token.kind == kind {
            Ok(token)
        } else {
            Err(CompileError::at(self.source, token.span.start, format!("expected {what}")))
        }
    }

    fn eat(&mut self, kind: &TokenKind) -> bool {
        if self.is(kind) {
            self.index += 1;
            true
        } else {
            false
        }
    }

    fn is(&self, kind: &TokenKind) -> bool {
        self.current().kind == *kind
    }

    fn is_ident(&self) -> bool {
        matches!(self.current().kind, TokenKind::Ident(_))
    }

    fn peek_assign(&self) -> bool {
        let Some(token) = self.tokens.get(self.index + 1) else {
            return false;
        };
        matches!(
            token.kind,
            TokenKind::Eq
                | TokenKind::PlusEq
                | TokenKind::MinusEq
                | TokenKind::StarEq
                | TokenKind::SlashEq
                | TokenKind::PercentEq
        )
    }

    fn current(&self) -> &Token {
        &self.tokens[self.index.min(self.tokens.len() - 1)]
    }

    fn bump(&mut self) -> Token {
        let token = self.current().clone();
        if self.index < self.tokens.len() {
            self.index += 1;
        }
        token
    }

    fn prev_span(&self) -> Span {
        self.tokens[self.index.saturating_sub(1)].span
    }
}
