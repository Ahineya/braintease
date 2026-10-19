mod associated;
mod cfg;
mod comprehension;

use crate::ast::*;
use crate::error::CompileError;
use crate::lexer::{Token, TokenKind};

pub(crate) const EXPECTED_ASSOCIATED_TYPE_NAME: &str =
    "expected an associated type name after '::'";

pub(crate) const EXPECTED_MEMBER_NAME: &str = "expected a member name after '.'";

pub fn parse(source: &str, tokens: &[Token]) -> Result<Module, CompileError> {
    parse_for_target(source, tokens, rcc_ir::target::Target::Ripple)
}

pub fn parse_for_target(
    source: &str,
    tokens: &[Token],
    target: rcc_ir::target::Target,
) -> Result<Module, CompileError> {
    let mut parser = Parser {
        target,
        source,
        tokens,
        index: 0,
        pending_gt: false,
        pending_lt: false,
        tolerant: false,
        errors: Vec::new(),
        comprehension_helpers: [false; 4],
    };
    parser.parse_module()
}

pub(crate) fn parse_expression(
    source: &str,
    tokens: &[Token],
    target: rcc_ir::target::Target,
) -> Result<(Expr, Vec<Import>), CompileError> {
    let mut parser = Parser {
        target,
        source,
        tokens,
        index: 0,
        pending_gt: false,
        pending_lt: false,
        tolerant: false,
        errors: Vec::new(),
        comprehension_helpers: [false; 4],
    };
    let expression = parser.parse_expr()?;
    parser.expect(
        &TokenKind::Eof,
        "end of macro expression (trailing tokens are not allowed)",
    )?;
    let mut imports = Vec::new();
    parser.add_comprehension_import(&mut imports);
    Ok((expression, imports))
}

/// Parse for the language server. A missing name after `.`, or a missing `;`
/// before the next statement, is reported and the rest of the file is kept.
pub fn parse_for_ide(source: &str, tokens: &[Token]) -> (Module, Vec<CompileError>) {
    let mut parser = Parser {
        target: rcc_ir::target::Target::Ripple,
        source,
        tokens,
        index: 0,
        pending_gt: false,
        pending_lt: false,
        tolerant: true,
        errors: Vec::new(),
        comprehension_helpers: [false; 4],
    };
    match parser.parse_module() {
        Ok(module) => (module, parser.errors),
        Err(err) => {
            parser.errors.push(err);
            (
                Module {
                    adts: Vec::new(),
                    structs: Vec::new(),
                    impls: Vec::new(),
                    traits: Vec::new(),
                    trait_scopes: Vec::new(),
                    consts: Vec::new(),
                    functions: Vec::new(),
                    externs: Vec::new(),
                    imports: Vec::new(),
                    macro_imports: Vec::new(),
                    macro_exports: Vec::new(),
                    item_macros: Vec::new(),
                    proc_macros: Vec::new(),
                    macro_expansion: None,
                },
                parser.errors,
            )
        }
    }
}

struct Parser<'a> {
    target: rcc_ir::target::Target,
    source: &'a str,
    tokens: &'a [Token],
    index: usize,
    /// One `>` left over from a `>>` that closed a generic argument.
    pending_gt: bool,
    /// One `<` left from `<<` in a nested type argument.
    pending_lt: bool,
    /// Keep going after a missing member name or semicolon. The compiler does not.
    tolerant: bool,
    errors: Vec<CompileError>,
    /// Hygienic std/fp imports required by parsed comprehension expansions.
    comprehension_helpers: [bool; 4],
}

impl<'a> Parser<'a> {
    fn parse_module(&mut self) -> Result<Module, CompileError> {
        let mut adts = Vec::new();
        let mut structs = Vec::new();
        let mut impls = Vec::new();
        let mut traits = Vec::new();
        let mut consts = Vec::new();
        let mut functions = Vec::new();
        let mut externs = Vec::new();
        let mut imports = Vec::new();
        let mut macro_imports = Vec::new();
        let mut macro_exports = Vec::new();
        let mut item_macros = Vec::new();
        let mut proc_macros = Vec::new();
        let mut parsed_any = false;
        while !self.is(&TokenKind::Eof) {
            let mut enabled = true;
            let mut repr_c = false;
            let mut proc_macro = false;
            let macro_counts = (
                macro_imports.len(),
                item_macros.len(),
                proc_macros.len(),
                macro_exports.len(),
            );
            while self.is(&TokenKind::Hash) {
                let before = self.index;
                enabled &= self.cfg_attributes()?;
                if self.index == before {
                    if matches!(self.tokens.get(self.index + 2).map(|t| &t.kind), Some(TokenKind::Ident(n)) if n == "proc_macro")
                    {
                        if proc_macro {
                            return Err(CompileError::at(
                                self.source,
                                self.current().span.start,
                                "duplicate proc_macro attribute",
                            ));
                        }
                        self.expect(&TokenKind::Hash, "'#'")?;
                        self.expect(&TokenKind::LBracket, "'['")?;
                        self.expect_ident()?;
                        self.expect(&TokenKind::RBracket, "']'")?;
                        proc_macro = true;
                        continue;
                    }
                    if repr_c {
                        return Err(CompileError::at(
                            self.source,
                            self.current().span.start,
                            "duplicate repr attribute",
                        ));
                    }
                    self.parse_repr_c()?;
                    repr_c = true;
                }
            }
            let trait_count = traits.len();
            let saved_helpers = self.comprehension_helpers;
            let counts = (
                adts.len(),
                structs.len(),
                impls.len(),
                consts.len(),
                functions.len(),
                externs.len(),
                imports.len(),
            );
            parsed_any = true;
            if repr_c {
                let exported = self.eat(&TokenKind::Export);
                let mut item = self.parse_struct()?;
                item.repr_c = true;
                item.exported = exported;
                structs.push(item);
            } else if self.is(&TokenKind::Type) {
                adts.push(self.parse_adt()?);
            } else if self.is(&TokenKind::Struct) {
                structs.push(self.parse_struct()?);
            } else if self.is(&TokenKind::Trait) {
                traits.push(self.parse_trait(false)?);
            } else if self.is(&TokenKind::Impl) {
                impls.push(self.parse_impl()?);
            } else if self.is(&TokenKind::Const) {
                consts.push(self.parse_const()?);
            } else if self.is(&TokenKind::Export)
                && self
                    .tokens
                    .get(self.index + 1)
                    .is_some_and(|t| t.kind == TokenKind::Import)
            {
                self.bump();
                if !matches!(self.tokens.get(self.index+1).map(|t|&t.kind),Some(TokenKind::Ident(n)) if n=="macro")
                {
                    return Err(CompileError::at(
                        self.source,
                        self.current().span.start,
                        "only macro reexports are supported here",
                    ));
                }
                let import = self.parse_import()?;
                macro_exports.extend(import.names.iter().map(|n| n.alias.clone()));
                macro_imports.push(import);
            } else if self.is(&TokenKind::Export) {
                if self
                    .tokens
                    .get(self.index + 1)
                    .is_some_and(|t| t.kind == TokenKind::Trait)
                {
                    self.bump();
                    traits.push(self.parse_trait(true)?);
                } else {
                    self.parse_exported(&mut adts, &mut structs, &mut consts, &mut functions)?;
                }
            } else if self.is(&TokenKind::Extern) {
                externs.extend(self.parse_extern_block()?);
            } else if self.is(&TokenKind::Fn) || self.is(&TokenKind::Async) {
                functions.push(self.parse_function()?);
            } else if self.is(&TokenKind::Import) {
                let is_macro = matches!(self.tokens.get(self.index + 1).map(|t| &t.kind), Some(TokenKind::Ident(n)) if n == "macro");
                let import = self.parse_import()?;
                if is_macro {
                    macro_imports.push(import);
                } else {
                    imports.push(import);
                }
            } else if matches!(&self.current().kind, TokenKind::Ident(_))
                && self
                    .tokens
                    .get(self.index + 1)
                    .is_some_and(|t| t.kind == TokenKind::Not)
            {
                let (name, name_span) = self.expect_ident()?;
                item_macros.push(self.parse_macro_invocation(name, name_span)?);
                self.eat(&TokenKind::Semi);
            } else {
                let token = self.bump();
                return Err(CompileError::at(
                    self.source,
                    token.span.start,
                    "expected `fn`, `type`, `struct`, `trait`, `impl`, or `const`",
                ));
            }
            if proc_macro {
                if repr_c || functions.len() != counts.4 + 1 {
                    return Err(CompileError::at(
                        self.source,
                        self.current().span.start,
                        "proc_macro requires a function",
                    ));
                }
                proc_macros.push(functions.last().unwrap().name.clone());
            }
            if !enabled {
                macro_exports.truncate(macro_counts.3);
                macro_imports.truncate(macro_counts.0);
                item_macros.truncate(macro_counts.1);
                proc_macros.truncate(macro_counts.2);
                self.comprehension_helpers = saved_helpers;
                traits.truncate(trait_count);
                adts.truncate(counts.0);
                structs.truncate(counts.1);
                impls.truncate(counts.2);
                consts.truncate(counts.3);
                functions.truncate(counts.4);
                externs.truncate(counts.5);
                imports.truncate(counts.6);
            }
        }
        if !parsed_any
            && functions.is_empty()
            && adts.is_empty()
            && structs.is_empty()
            && consts.is_empty()
            && externs.is_empty()
            && impls.is_empty()
            && imports.is_empty()
        {
            return Err(CompileError::at(self.source, 0, "expected a function"));
        }
        self.add_comprehension_import(&mut imports);
        Ok(Module {
            adts,
            structs,
            impls,
            traits,
            trait_scopes: Vec::new(),
            consts,
            functions,
            externs,
            imports,
            macro_imports,
            macro_exports,
            item_macros,
            proc_macros,
            macro_expansion: None,
        })
    }

    fn parse_function(&mut self) -> Result<Function, CompileError> {
        let is_async = self.eat(&TokenKind::Async);
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
        let ret = if is_async {
            Some(ParsedTy::Named {
                span: Span::empty(),
                name: "Async".into(),
                args: vec![ret.unwrap_or(ParsedTy::Named {
                    span: name_tok.1,
                    name: "unit".into(),
                    args: Vec::new(),
                })],
            })
        } else {
            ret
        };
        let body = self.parse_block()?;
        Ok(Function {
            is_async,
            name: name_tok.0,
            name_span: name_tok.1,
            type_params,
            params,
            ret,
            body,
            c_export: false,
            exported: false,
        })
    }

    fn parse_exported(
        &mut self,
        adts: &mut Vec<Adt>,
        structs: &mut Vec<StructItem>,
        consts: &mut Vec<Const>,
        functions: &mut Vec<Function>,
    ) -> Result<(), CompileError> {
        self.expect(&TokenKind::Export, "export")?;
        if self.is(&TokenKind::Extern) {
            self.expect(&TokenKind::Extern, "`extern`")?;
            self.expect_abi_c()?;
            let mut func = self.parse_function()?;
            func.c_export = true;
            func.exported = true;
            functions.push(func);
            return Ok(());
        }
        if self.is(&TokenKind::Hash) {
            self.parse_repr_c()?;
            let mut item = self.parse_struct()?;
            item.repr_c = true;
            item.exported = true;
            structs.push(item);
            return Ok(());
        }
        if self.is(&TokenKind::Type) {
            let mut adt = self.parse_adt()?;
            adt.exported = true;
            adts.push(adt);
            return Ok(());
        }
        if self.is(&TokenKind::Struct) {
            let mut item = self.parse_struct()?;
            item.exported = true;
            structs.push(item);
            return Ok(());
        }
        if self.is(&TokenKind::Const) {
            let mut item = self.parse_const()?;
            item.exported = true;
            consts.push(item);
            return Ok(());
        }
        if self.is(&TokenKind::Fn) || self.is(&TokenKind::Async) {
            let mut func = self.parse_function()?;
            func.exported = true;
            functions.push(func);
            return Ok(());
        }
        let token = self.bump();
        Err(CompileError::at(
            self.source,
            token.span.start,
            "expected `fn`, `type`, `struct`, `const`, or `extern` after `export`",
        ))
    }

    fn parse_repr_c(&mut self) -> Result<(), CompileError> {
        self.expect(&TokenKind::Hash, "'#'")?;
        self.expect(&TokenKind::LBracket, "'['")?;
        let (name, span) = self.expect_ident()?;
        if name != "repr" {
            return Err(CompileError::at(
                self.source,
                span.start,
                "unknown attribute",
            ));
        }
        self.expect(&TokenKind::LParen, "'('")?;
        let (kind, span) = self.expect_ident()?;
        if kind != "C" {
            return Err(CompileError::at(
                self.source,
                span.start,
                "unknown representation",
            ));
        }
        self.expect(&TokenKind::RParen, "')'")?;
        self.expect(&TokenKind::RBracket, "']'")?;
        Ok(())
    }

    fn parse_import(&mut self) -> Result<Import, CompileError> {
        let start = self.expect(&TokenKind::Import, "import")?;
        if matches!(&self.current().kind, TokenKind::Ident(n) if n == "macro") {
            self.bump();
        }
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut names = Vec::new();
        if !self.is(&TokenKind::RBrace) {
            loop {
                let (name, span) = self.expect_ident()?;
                let (alias, alias_span) = if self.eat(&TokenKind::As) {
                    self.expect_ident()?
                } else {
                    (name.clone(), span)
                };
                names.push(ImportItem {
                    name,
                    alias,
                    span,
                    alias_span,
                });
                if self.eat(&TokenKind::Comma) {
                    if self.is(&TokenKind::RBrace) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        // Empty imports link a dependency without exposing its declarations.
        // In particular, C libraries can retain their opaque internal ABI.
        let (word, word_span) = self.expect_ident()?;
        if word != "from" {
            return Err(CompileError::at(
                self.source,
                word_span.start,
                "expected `from`",
            ));
        }
        let path_tok = self.bump();
        let path = match path_tok.kind {
            TokenKind::Str(chars) => decode_path(&chars),
            _ => {
                return Err(CompileError::at(
                    self.source,
                    path_tok.span.start,
                    "expected a path",
                ))
            }
        };
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Import {
            span: start.span,
            names,
            path,
            path_span: path_tok.span,
        })
    }

    fn parse_extern_block(&mut self) -> Result<Vec<ExternDecl>, CompileError> {
        self.expect(&TokenKind::Extern, "extern")?;
        self.expect_abi_c()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut decls = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let enabled = self.cfg_attributes()?;
            let declaration = self.parse_extern_fn()?;
            if enabled {
                decls.push(declaration);
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(decls)
    }

    fn parse_extern_fn(&mut self) -> Result<ExternDecl, CompileError> {
        self.expect(&TokenKind::Fn, "fn")?;
        let (name, name_span) = self.expect_ident()?;
        self.expect(&TokenKind::LParen, "'('")?;
        let mut params = Vec::new();
        let mut variadic = false;
        if !self.is(&TokenKind::RParen) {
            loop {
                if self.eat(&TokenKind::Ellipsis) {
                    variadic = true;
                    break;
                }
                params.push(self.parse_param()?);
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
        if variadic && params.is_empty() {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "a variadic function needs a named parameter",
            ));
        }
        let ret = if self.eat(&TokenKind::Arrow) {
            Some(self.parse_type()?)
        } else {
            None
        };
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(ExternDecl {
            name,
            name_span,
            params,
            ret,
            variadic,
        })
    }

    fn expect_abi_c(&mut self) -> Result<(), CompileError> {
        let token = self.bump();
        match &token.kind {
            TokenKind::Str(chars) if chars.len() == 1 && chars[0] == u16::from(b'C') => Ok(()),
            _ => Err(CompileError::at(
                self.source,
                token.span.start,
                "expected `\"C\"`",
            )),
        }
    }

    fn parse_trait(&mut self, exported: bool) -> Result<Trait, CompileError> {
        let start = self.expect(&TokenKind::Trait, "trait")?.span;
        let (name, name_span) = self.expect_ident()?;
        if name == "Self" {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "Self is reserved for the implementing type",
            ));
        }
        let mut arity = 0;
        if self.eat_lt() {
            loop {
                let (slot, span) = self.expect_ident()?;
                if slot != "_" {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "trait parameters are not supported; use `_` slots for a constructor trait",
                    ));
                }
                arity += 1;
                if !self.eat(&TokenKind::Comma) {
                    break;
                }
            }
            self.expect_gt()?;
        }
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut methods = Vec::new();
        let mut associated_types = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let enabled = self.cfg_attributes()?;
            if self.is(&TokenKind::Type) {
                let item = self.parse_associated_type(true)?;
                if enabled {
                    associated_types.push(item);
                }
                continue;
            }
            let method = self.parse_method(true)?;
            if arity == 0 && method.receiver.is_none() {
                return Err(CompileError::at(
                    self.source,
                    method.name_span.start,
                    "a trait method needs self, &self, or &mut self",
                ));
            }
            if arity > 0 && method.receiver.is_some() {
                return Err(CompileError::at(
                    self.source,
                    method.name_span.start,
                    "constructor trait operations take applied values, not a self receiver",
                ));
            }
            if enabled {
                methods.push(method);
            }
        }
        let end = self.expect(&TokenKind::RBrace, "'}'")?.span;
        Ok(Trait {
            arity,
            span: start.to(end),
            name,
            name_span,
            exported,
            methods,
            associated_types,
        })
    }

    fn parse_impl(&mut self) -> Result<Impl, CompileError> {
        let span = self.expect(&TokenKind::Impl, "impl")?.span;
        let type_params = self.parse_generic_params()?;
        let first = self.parse_type()?;
        let (trait_name, ty) = if self.eat(&TokenKind::For) {
            let ParsedTy::Named { name, span, args } = first else {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    "expected a trait name",
                ));
            };
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    "trait arguments are not supported",
                ));
            }
            (Some((name, span)), self.parse_type()?)
        } else {
            (None, first)
        };
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut items = Vec::new();
        let mut associated_types = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let enabled = self.cfg_attributes()?;
            self.eat(&TokenKind::Export);
            if self.is(&TokenKind::Type) {
                if trait_name.is_none() {
                    return Err(CompileError::at(
                        self.source,
                        self.current().span.start,
                        "associated types require a trait impl",
                    ));
                }
                let item = self.parse_associated_type(false)?;
                if enabled {
                    associated_types.push(item);
                }
                continue;
            }
            let method = self.parse_method(false)?;
            if enabled {
                items.push(method);
            }
        }
        let end = self.expect(&TokenKind::RBrace, "'}'")?.span;
        Ok(Impl {
            trait_name,
            span: span.to(end),
            type_params,
            ty,
            items,
            associated_types,
        })
    }

    fn parse_method(&mut self, signature: bool) -> Result<ImplItem, CompileError> {
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
        let ret = if self.eat(&TokenKind::Arrow) {
            Some(self.parse_type()?)
        } else {
            None
        };
        let body = if signature {
            self.expect(&TokenKind::Semi, "';' (trait methods have signatures only)")?;
            Block {
                span: name_span,
                stmts: Vec::new(),
                tail: None,
            }
        } else {
            self.parse_block()?
        };
        Ok(ImplItem {
            name,
            name_span,
            type_params,
            receiver,
            params,
            ret,
            body,
        })
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
        if name == "Self" {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "Self is reserved for the implementing type",
            ));
        }
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
        Ok(Adt {
            name,
            name_span,
            params,
            ctors,
            exported: false,
        })
    }

    fn parse_generic_params(&mut self) -> Result<Vec<TypeParam>, CompileError> {
        if !self.eat_lt() {
            return Ok(Vec::new());
        }
        let mut params = Vec::new();
        loop {
            let (name, span) = self.expect_ident()?;
            if name == "Self" {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    "Self is reserved for the implementing type",
                ));
            }
            let mut arity = 0;
            if self.eat_lt() {
                loop {
                    let (slot, slot_span) = self.expect_ident()?;
                    if slot != "_" {
                        return Err(CompileError::at(
                            self.source,
                            slot_span.start,
                            "constructor parameters require `_` slots, such as F<_>",
                        ));
                    }
                    arity += 1;
                    if !self.eat(&TokenKind::Comma) {
                        break;
                    }
                }
                self.expect_gt()?;
            }
            let mut bounds = Vec::new();
            if self.eat(&TokenKind::Colon) {
                loop {
                    bounds.push(self.expect_ident()?);
                    if self.is(&TokenKind::Lt) {
                        return Err(CompileError::at(self.source, self.current().span.start, "associated type equality constraints and generic trait bounds are not supported"));
                    }
                    if !self.eat(&TokenKind::Plus) {
                        break;
                    }
                }
            }
            params.push(TypeParam {
                name,
                span,
                bounds,
                arity,
            });
            if self.eat(&TokenKind::Comma) {
                continue;
            }
            break;
        }
        self.expect_gt()?;
        Ok(params)
    }

    /// `name<ty, ...>(...)`. Anything else, including `a < b`, is left for the
    /// comparison parser.
    fn try_generic_call(
        &mut self,
        callee: String,
        span: Span,
    ) -> Result<Option<Expr>, CompileError> {
        if !self.is(&TokenKind::Lt) && !self.is(&TokenKind::Shl) {
            return Ok(None);
        }
        let saved = self.index;
        let saved_gt = self.pending_gt;
        let saved_lt = self.pending_lt;
        self.eat_lt();
        if !self.starts_type() {
            self.index = saved;
            self.pending_gt = saved_gt;
            self.pending_lt = saved_lt;
            return Ok(None);
        }
        let mut type_args = Vec::new();
        loop {
            match self.parse_type() {
                Ok(ty) => type_args.push(ty),
                Err(_) => {
                    self.index = saved;
                    self.pending_gt = saved_gt;
                    self.pending_lt = saved_lt;
                    return Ok(None);
                }
            }
            if self.eat(&TokenKind::Comma) {
                if !self.starts_type() {
                    self.index = saved;
                    self.pending_gt = saved_gt;
                    self.pending_lt = saved_lt;
                    return Ok(None);
                }
                continue;
            }
            break;
        }
        if !self.eat_gt() || !self.is(&TokenKind::LParen) {
            self.index = saved;
            self.pending_gt = saved_gt;
            self.pending_lt = saved_lt;
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
        Ok(Some(Expr {
            span,
            kind: ExprKind::Call {
                callee,
                callee_span: span,
                type_args,
                args,
            },
        }))
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
        Ok(Ctor {
            name,
            name_span,
            fields,
        })
    }

    fn parse_const(&mut self) -> Result<Const, CompileError> {
        self.expect(&TokenKind::Const, "const")?;
        let (name, name_span) = self.expect_ident()?;
        self.expect(&TokenKind::Colon, "':'")?;
        let ty = self.parse_type()?;
        self.expect(&TokenKind::Eq, "'='")?;
        let init = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        Ok(Const {
            name,
            name_span,
            ty,
            init,
            exported: false,
        })
    }

    fn parse_struct(&mut self) -> Result<StructItem, CompileError> {
        self.expect(&TokenKind::Struct, "struct")?;
        let (name, name_span) = self.expect_ident()?;
        if name == "Self" {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "Self is reserved for the implementing type",
            ));
        }
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut fields = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let (field_name, field_span) = self.expect_ident()?;
            self.expect(&TokenKind::Colon, "':'")?;
            let ty = self.parse_type()?;
            fields.push(StructField {
                name: field_name,
                name_span: field_span,
                ty,
            });
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        if fields.is_empty() {
            return Err(CompileError::at(
                self.source,
                name_span.start,
                "a struct needs a field",
            ));
        }
        Ok(StructItem {
            name,
            name_span,
            fields,
            repr_c: false,
            exported: false,
        })
    }

    fn parse_param(&mut self) -> Result<Param, CompileError> {
        let pat = self.parse_pat()?;
        let (name, name_span) = match &pat {
            Pat::Name { name, span } if name != "_" => (name.clone(), *span),
            _ => (String::new(), pat.span()),
        };
        let ty = if self.eat(&TokenKind::Colon) {
            Some(self.parse_type()?)
        } else {
            None
        };
        Ok(Param {
            name,
            name_span,
            ty,
            pat,
        })
    }

    fn parse_c_fn_type(&mut self) -> Result<ParsedTy, CompileError> {
        let span = self.expect(&TokenKind::Extern, "`extern`")?.span;
        self.expect_abi_c()?;
        self.expect(&TokenKind::Fn, "`fn`")?;
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
            ParsedTy::Named {
                span,
                name: "unit".to_string(),
                args: Vec::new(),
            }
        };
        Ok(ParsedTy::CFn {
            span,
            params,
            ret: Box::new(ret),
        })
    }

    fn parse_type_atom(&mut self) -> Result<ParsedTy, CompileError> {
        if self.is(&TokenKind::Extern) {
            return self.parse_c_fn_type();
        }
        if self.eat(&TokenKind::LParen) {
            let span = self.prev_span();
            if self.eat(&TokenKind::RParen) {
                return Ok(ParsedTy::Named {
                    span,
                    name: "unit".to_string(),
                    args: Vec::new(),
                });
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
            return Ok(ParsedTy::Array {
                span,
                elem: Box::new(elem),
                len: Box::new(len),
            });
        }
        if self.eat(&TokenKind::Amp) {
            let span = self.prev_span();
            let mutable = self.eat(&TokenKind::Mut);
            let inner = self.parse_type()?;
            return Ok(ParsedTy::Ref {
                span,
                mutable,
                inner: Box::new(inner),
            });
        }
        if self.eat(&TokenKind::Star) {
            let span = self.prev_span();
            let mutable = if self.eat(&TokenKind::Mut) {
                true
            } else if self.eat(&TokenKind::Const) {
                false
            } else {
                return Err(CompileError::at(
                    self.source,
                    span.start,
                    "expected `const` or `mut` after `*`",
                ));
            };
            let inner = self.parse_type()?;
            return Ok(ParsedTy::Ptr {
                span,
                mutable,
                inner: Box::new(inner),
            });
        }
        let once = matches!(&self.current().kind, TokenKind::Ident(name) if name == "once")
            && self
                .tokens
                .get(self.index + 1)
                .is_some_and(|t| t.kind == TokenKind::Fn);
        if once {
            self.bump();
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
                ParsedTy::Named {
                    span,
                    name: "unit".to_string(),
                    args: Vec::new(),
                }
            };
            return Ok(ParsedTy::Func {
                once,
                span,
                params,
                ret: Box::new(ret),
            });
        }
        let (name, span) = self.expect_ident()?;
        if self.is(&TokenKind::Not) {
            return Err(CompileError::at(
                self.source,
                span.start,
                "macros are not supported in type positions",
            ));
        }
        let mut args = Vec::new();
        // A comprehension annotation can be followed by its binding arrow.
        // `<-` remains two tokens so comparisons such as `x<-1` keep working.
        let binding_arrow = self.is(&TokenKind::Lt)
            && self.tokens.get(self.index + 1).is_some_and(|t| {
                t.kind == TokenKind::Minus && self.current().span.end == t.span.start
            });
        if !binding_arrow && self.eat_lt() {
            loop {
                args.push(self.parse_type()?);
                if self.eat(&TokenKind::Comma) {
                    continue;
                }
                break;
            }
            self.expect_gt()?;
        }
        Ok(ParsedTy::Named { name, span, args })
    }

    fn parse_lambda(&mut self, span: Span, once: bool) -> Result<Expr, CompileError> {
        if self.is(&TokenKind::Lt) {
            return Err(CompileError::at(
                self.source,
                span.start,
                "generic lambdas are not implemented yet",
            ));
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
            Expr {
                span: block.span,
                kind: ExprKind::Block(Box::new(block)),
            }
        };
        Ok(Expr {
            span: span.to(body.span),
            kind: ExprKind::Lambda {
                once,
                params,
                body: Box::new(body),
            },
        })
    }

    fn parse_block(&mut self) -> Result<Block, CompileError> {
        let open = self.expect(&TokenKind::LBrace, "'{'")?;
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
            // A bare block-shaped statement ends before the next statement's
            // unary prefix (`*ptr`, `&value`, `-value`). Parenthesized block
            // expressions still enter the normal binary-expression parser.
            let expr = if matches!(
                self.current().kind,
                TokenKind::If
                    | TokenKind::While
                    | TokenKind::For
                    | TokenKind::Match
                    | TokenKind::LBrace
                    | TokenKind::Unsafe
            ) {
                self.parse_postfix()?
            } else {
                self.parse_expr()?
            };
            if self.is_assign_op() {
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
            if self.tolerant && !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
                self.errors
                    .push(CompileError::at(self.source, expr.span.end, "expected ';'"));
                stmts.push(Stmt::Expr(expr));
                continue;
            }
            tail = Some(expr);
            break;
        }
        let close = self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Block {
            span: open.span.to(close.span),
            stmts,
            tail,
        })
    }

    fn parse_let(&mut self) -> Result<Stmt, CompileError> {
        let keyword = self.expect(&TokenKind::Let, "let")?;
        let mutable = self.eat(&TokenKind::Mut);
        let pattern = self.parse_pat()?;
        let ty = if self.eat(&TokenKind::Colon) {
            Some(self.parse_type()?)
        } else {
            None
        };
        self.expect(&TokenKind::Eq, "'='")?;
        let init = self.parse_expr()?;
        let semi = self.expect(&TokenKind::Semi, "';'")?;
        Ok(Stmt::Let {
            span: keyword.span.to(semi.span),
            pattern,
            mutable,
            ty,
            init,
        })
    }

    fn parse_assign(&mut self) -> Result<Stmt, CompileError> {
        let (name, name_span) = self.expect_ident()?;
        let op = self.compound_op()?;
        let rhs = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        let value = match op {
            None => rhs,
            Some(op) => {
                let lhs = Expr {
                    span: name_span,
                    kind: ExprKind::Name(name.clone()),
                };
                self.binary(op, lhs, rhs)
            }
        };
        Ok(Stmt::Assign {
            name,
            name_span,
            value,
        })
    }

    fn finish_place_assign(&mut self, expr: Expr) -> Result<Stmt, CompileError> {
        if let ExprKind::Deref(_) = &expr.kind {
            let ExprKind::Deref(inner) = expr.kind.clone() else {
                unreachable!()
            };
            let op = self.compound_op()?;
            let rhs = self.parse_expr()?;
            self.expect(&TokenKind::Semi, "';'")?;
            let value = self.compound_value(
                op,
                Expr {
                    span: expr.span,
                    kind: ExprKind::Deref(inner.clone()),
                },
                rhs,
            );
            return Ok(Stmt::DerefAssign { ptr: *inner, value });
        }
        // Keep the direct named-field/index paths. More deeply projected places
        // use the same COW-safe address calculation as an explicit mutable borrow.
        if matches!(&expr.kind, ExprKind::Field { .. } | ExprKind::Index { .. }) {
            let mut base = match &expr.kind {
                ExprKind::Index { base, .. } => base.as_ref(),
                _ => &expr,
            };
            while let ExprKind::Field { base: parent, .. } = &base.kind {
                base = parent;
            }
            if !matches!(base.kind, ExprKind::Name(_)) {
                return self.finish_projected_assign(expr);
            }
        }
        if let ExprKind::Field { .. } = &expr.kind {
            let lhs = expr.clone();
            let mut fields = Vec::new();
            let mut cursor = expr;
            let root = loop {
                match cursor.kind {
                    ExprKind::Field { base, name, .. } => {
                        fields.push(name);
                        cursor = *base;
                    }
                    ExprKind::Name(name) => break (name, cursor.span),
                    _ => {
                        return Err(CompileError::at(
                            self.source,
                            cursor.span.start,
                            "field assignment needs a name",
                        ));
                    }
                }
            };
            fields.reverse();
            let op = self.compound_op()?;
            let rhs = self.parse_expr()?;
            self.expect(&TokenKind::Semi, "';'")?;
            let value = self.compound_value(op, lhs, rhs);
            return Ok(Stmt::FieldAssign {
                name: root.0,
                name_span: root.1,
                fields,
                value,
            });
        }
        self.finish_index_assign(expr)
    }

    fn finish_projected_assign(&mut self, lhs: Expr) -> Result<Stmt, CompileError> {
        let op = self.compound_op()?;
        let rhs = self.parse_expr()?;
        let end = self.expect(&TokenKind::Semi, "';'")?.span;
        let span = lhs.span.to(end);
        let name = format!("$place${}", span.start);
        let pointer = Expr {
            span: lhs.span,
            kind: ExprKind::Name(name.clone()),
        };
        let read = Expr {
            span: lhs.span,
            kind: ExprKind::Deref(Box::new(pointer.clone())),
        };
        let value = self.compound_value(op, read, rhs);
        Ok(Stmt::Expr(Expr {
            span,
            kind: ExprKind::Block(Box::new(Block {
                span,
                stmts: vec![
                    Stmt::Let {
                        span: lhs.span,
                        mutable: false,
                        pattern: Pat::Name {
                            name,
                            span: Span::empty(),
                        },
                        ty: None,
                        init: Expr {
                            span: lhs.span,
                            kind: ExprKind::Borrow {
                                mutable: true,
                                temporary: false,
                                expr: Box::new(lhs),
                            },
                        },
                    },
                    Stmt::DerefAssign {
                        ptr: pointer,
                        value,
                    },
                ],
                tail: None,
            })),
        }))
    }

    fn finish_index_assign(&mut self, expr: Expr) -> Result<Stmt, CompileError> {
        let lhs = expr.clone();
        let ExprKind::Index { base, index } = expr.kind else {
            return Err(CompileError::at(
                self.source,
                expr.span.start,
                "expected an index before '='",
            ));
        };
        let mut fields = Vec::new();
        let mut cursor = *base;
        let (name, name_span) = loop {
            match cursor.kind {
                ExprKind::Name(name) => break (name, cursor.span),
                ExprKind::Field { base, name, .. } => {
                    fields.push(name);
                    cursor = *base;
                }
                _ => {
                    return Err(CompileError::at(
                        self.source,
                        cursor.span.start,
                        "index assignment needs a name",
                    ));
                }
            }
        };
        fields.reverse();
        let op = self.compound_op()?;
        let rhs = self.parse_expr()?;
        self.expect(&TokenKind::Semi, "';'")?;
        let value = self.compound_value(op, lhs, rhs);
        Ok(Stmt::IndexAssign {
            name,
            name_span,
            fields,
            index: *index,
            value,
        })
    }

    fn compound_value(&self, op: Option<BinOp>, lhs: Expr, rhs: Expr) -> Expr {
        match op {
            None => rhs,
            Some(op) => self.binary(op, lhs, rhs),
        }
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
            _ => {
                return Err(CompileError::at(
                    self.source,
                    token.span.start,
                    "expected '='",
                ))
            }
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
        let mut expr = self.parse_bitor()?;
        while self.eat(&TokenKind::AndAnd) {
            let rhs = self.parse_bitor()?;
            expr = self.binary(BinOp::And, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_bitor(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_bitxor()?;
        while self.eat(&TokenKind::Pipe) {
            let rhs = self.parse_bitxor()?;
            expr = self.binary(BinOp::BitOr, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_bitxor(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_bitand()?;
        while self.eat(&TokenKind::Caret) {
            let rhs = self.parse_bitand()?;
            expr = self.binary(BinOp::BitXor, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_bitand(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_equality()?;
        while self.eat(&TokenKind::Amp) {
            let rhs = self.parse_equality()?;
            expr = self.binary(BinOp::BitAnd, expr, rhs);
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
            let rhs = self.parse_shift()?;
            expr = self.binary(op, expr, rhs);
        }
        Ok(expr)
    }

    fn parse_range(&mut self) -> Result<Expr, CompileError> {
        let expr = self.parse_shift()?;
        let inclusive = self.eat(&TokenKind::DotDotEq);
        if inclusive || self.eat(&TokenKind::DotDot) {
            let end = self.parse_shift()?;
            return Ok(Expr {
                span: expr.span.to(end.span),
                kind: ExprKind::Range {
                    start: Box::new(expr),
                    end: Box::new(end),
                    inclusive,
                },
            });
        }
        Ok(expr)
    }

    fn parse_shift(&mut self) -> Result<Expr, CompileError> {
        let mut expr = self.parse_term()?;
        loop {
            let op = if self.eat(&TokenKind::Shl) {
                BinOp::Shl
            } else if self.eat(&TokenKind::Shr) {
                BinOp::Shr
            } else {
                break;
            };
            let rhs = self.parse_term()?;
            expr = self.binary(op, expr, rhs);
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
        if self.eat(&TokenKind::Star) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span: span.to(expr.span),
                kind: ExprKind::Deref(Box::new(expr)),
            });
        }
        if self.eat(&TokenKind::Amp) {
            let span = self.prev_span();
            let mutable = self.eat(&TokenKind::Mut);
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span: span.to(expr.span),
                kind: ExprKind::Borrow {
                    mutable,
                    temporary: false,
                    expr: Box::new(expr),
                },
            });
        }
        if self.eat(&TokenKind::Minus) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span: span.to(expr.span),
                kind: ExprKind::Unary {
                    op: UnaryOp::Neg,
                    expr: Box::new(expr),
                },
            });
        }
        if self.eat(&TokenKind::Not) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span: span.to(expr.span),
                kind: ExprKind::Unary {
                    op: UnaryOp::Not,
                    expr: Box::new(expr),
                },
            });
        }
        if self.eat(&TokenKind::Tilde) {
            let span = self.prev_span();
            let expr = self.parse_unary()?;
            return Ok(Expr {
                span: span.to(expr.span),
                kind: ExprKind::Unary {
                    op: UnaryOp::BitNot,
                    expr: Box::new(expr),
                },
            });
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Result<Expr, CompileError> {
        // Parentheses explicitly make a block/if/match into a receiver. Keep
        // the bare-block statement boundary for unparenthesized expressions.
        let grouped = self.is(&TokenKind::LParen);
        let mut expr = self.parse_primary()?;
        loop {
            if expr.ends_with_block() && !grouped {
                break;
            }
            if let ExprKind::Name(name) = expr.kind.clone() {
                if let Some(call) = self.try_generic_call(name, expr.span)? {
                    expr = call;
                    expr.span = expr.span.to(self.prev_span());
                    continue;
                }
            }
            if let ExprKind::Name(ty_name) = expr.kind.clone() {
                if self.is(&TokenKind::LBrace) && self.looks_like_struct_body() {
                    expr = self.parse_struct_lit(ty_name, expr.span)?;
                    expr.span = expr.span.to(self.prev_span());
                    continue;
                }
                if self.eat(&TokenKind::ColonColon) {
                    let (method, _span) = self.expect_ident()?;
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
                    expr = Expr {
                        span: expr.span.to(self.prev_span()),
                        kind: ExprKind::Assoc {
                            ty_name,
                            method,
                            args,
                        },
                    };
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
                let callee_span = expr.span;
                expr = Expr {
                    span: callee_span,
                    kind: ExprKind::Call {
                        callee,
                        callee_span,
                        type_args: Vec::new(),
                        args,
                    },
                };
            } else if self.eat(&TokenKind::LBracket) {
                let inside = self.parse_expr()?;
                if let ExprKind::Range {
                    start,
                    end,
                    inclusive,
                } = inside.kind
                {
                    self.expect(&TokenKind::RBracket, "']'")?;
                    expr = Expr {
                        span: expr.span,
                        kind: ExprKind::Slice {
                            base: Box::new(expr),
                            start,
                            end,
                            inclusive,
                        },
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
                            kind: ExprKind::Index {
                                base: Box::new(expr),
                                index: Box::new(inside),
                            },
                        };
                    }
                }
            } else if self.eat(&TokenKind::Dot) {
                if self.eat(&TokenKind::Await) {
                    expr = Expr {
                        span: expr.span.to(self.prev_span()),
                        kind: ExprKind::Await(Box::new(expr)),
                    };
                    continue;
                }
                if !self.is_ident() {
                    let dot = self.prev_span();
                    if !self.tolerant {
                        return Err(CompileError::at(self.source, dot.end, "expected a name"));
                    }
                    self.errors.push(CompileError::at(
                        self.source,
                        dot.start,
                        EXPECTED_MEMBER_NAME,
                    ));
                    let base_span = expr.span;
                    let name_span = Span::new(dot.end, dot.end);
                    expr = Expr {
                        span: base_span.to(name_span),
                        kind: ExprKind::Field {
                            base: Box::new(expr),
                            name: String::new(),
                            name_span,
                        },
                    };
                    continue;
                }
                let (method, span) = self.expect_ident()?;
                if !self.is(&TokenKind::LParen) {
                    let base_span = expr.span;
                    expr = Expr {
                        span: base_span.to(span),
                        kind: ExprKind::Field {
                            base: Box::new(expr),
                            name: method,
                            name_span: span,
                        },
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
                let base_span = expr.span;
                expr = Expr {
                    span: base_span.to(self.prev_span()),
                    kind: ExprKind::Method {
                        receiver: Box::new(expr),
                        resolved: method.clone(),
                        method,
                        method_span: span,
                        args,
                    },
                };
            } else if self.eat(&TokenKind::Question) {
                expr = Expr {
                    span: expr.span,
                    kind: ExprKind::Try(Box::new(expr)),
                };
            } else if self.eat(&TokenKind::As) {
                let ty = self.parse_type()?;
                expr = Expr {
                    span: expr.span,
                    kind: ExprKind::Cast {
                        expr: Box::new(expr),
                        ty,
                    },
                };
            } else {
                break;
            }
            expr.span = expr.span.to(self.prev_span());
        }
        Ok(expr)
    }

    fn parse_primary(&mut self) -> Result<Expr, CompileError> {
        let token = self.bump();
        match token.kind.clone() {
            TokenKind::Int(value) => Ok(Expr {
                span: token.span,
                kind: ExprKind::Int(value),
            }),
            TokenKind::Float(text) => Ok(Expr {
                span: token.span,
                kind: ExprKind::Float(text),
            }),
            TokenKind::Str(chars) => Ok(Expr {
                span: token.span,
                kind: ExprKind::Str(chars),
            }),
            TokenKind::CStr(chars) => Ok(Expr {
                span: token.span,
                kind: ExprKind::CStr(chars),
            }),
            TokenKind::Char(scalar) => Ok(Expr {
                span: token.span,
                kind: ExprKind::Char(scalar),
            }),
            TokenKind::True => Ok(Expr {
                span: token.span,
                kind: ExprKind::Bool(true),
            }),
            TokenKind::False => Ok(Expr {
                span: token.span,
                kind: ExprKind::Bool(false),
            }),
            TokenKind::Ident(name) if name == "once" && self.is(&TokenKind::Fn) => {
                self.bump();
                self.parse_lambda(token.span, true)
            }
            TokenKind::Ident(name) if self.is(&TokenKind::Not) => {
                let invocation = self.parse_macro_invocation(name, token.span)?;
                Ok(Expr {
                    span: invocation.span,
                    kind: ExprKind::Macro(invocation),
                })
            }
            TokenKind::Ident(name) => Ok(Expr {
                span: token.span,
                kind: ExprKind::Name(name),
            }),
            TokenKind::Fn => self.parse_lambda(token.span, false),
            TokenKind::If => self.parse_if(token.span),
            TokenKind::While => self.parse_while(token.span),
            TokenKind::Match => self.parse_match(token.span),
            TokenKind::For => self.parse_for(token.span),
            TokenKind::LParen => self.parse_paren(token.span),
            TokenKind::LBracket => self.parse_array(token.span),
            TokenKind::Unsafe => {
                let block = self.parse_block()?;
                Ok(Expr {
                    span: token.span.to(block.span),
                    kind: ExprKind::Unsafe(Box::new(block)),
                })
            }
            TokenKind::LBrace => {
                self.index -= 1;
                let block = self.parse_block()?;
                Ok(Expr {
                    span: block.span,
                    kind: ExprKind::Block(Box::new(block)),
                })
            }
            other => Err(self.unexpected(&token, &other)),
        }
    }

    fn parse_macro_invocation(
        &mut self,
        name: String,
        name_span: Span,
    ) -> Result<crate::macros::syntax::Invocation, CompileError> {
        self.expect(&TokenKind::Not, "'!'")?;
        let token = self.bump();
        let TokenKind::MacroBody(group) = token.kind else {
            return Err(CompileError::at(
                self.source,
                token.span.start,
                "expected a delimited macro body",
            ));
        };
        Ok(crate::macros::syntax::Invocation {
            name,
            name_span,
            span: name_span.to(token.span),
            group,
            input: None,
        })
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
        Ok(Expr {
            span: span.to(self.prev_span()),
            kind: ExprKind::Array(elems),
        })
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
                Some(Box::new(Expr {
                    span: block.span,
                    kind: ExprKind::Block(Box::new(block)),
                }))
            }
        } else {
            None
        };
        Ok(Expr {
            span: span.to(self.prev_span()),
            kind: ExprKind::If {
                cond: Box::new(cond),
                then_body: Box::new(then_body),
                else_body,
            },
        })
    }

    fn parse_match(&mut self, span: Span) -> Result<Expr, CompileError> {
        let scrutinee = self.parse_expr()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut arms = Vec::new();
        while !self.is(&TokenKind::RBrace) && !self.is(&TokenKind::Eof) {
            let pat = self.parse_pat()?;
            let guard = if self.eat(&TokenKind::If) {
                Some(self.parse_expr()?)
            } else {
                None
            };
            self.expect(&TokenKind::FatArrow, "'=>'")?;
            let body = self.parse_expr()?;
            self.eat(&TokenKind::Comma);
            arms.push(Arm { pat, guard, body });
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        if arms.is_empty() {
            return Err(CompileError::at(
                self.source,
                span.start,
                "match has no arms",
            ));
        }
        Ok(Expr {
            span: span.to(self.prev_span()),
            kind: ExprKind::Match {
                scrutinee: Box::new(scrutinee),
                arms,
            },
        })
    }

    /// `{ field` followed by `:`, `,`, or `}` is a struct literal. A match arm is `{ Name =>`.
    fn looks_like_struct_body(&self) -> bool {
        let Some(TokenKind::Ident(_)) = self.tokens.get(self.index + 1).map(|token| &token.kind)
        else {
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
            let (field, field_span) = self.expect_ident()?;
            let value = if self.eat(&TokenKind::Colon) {
                self.parse_expr()?
            } else {
                Expr {
                    span: field_span,
                    kind: ExprKind::Name(field.clone()),
                }
            };
            fields.push((field, value));
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Expr {
            span,
            kind: ExprKind::StructLit { name, fields },
        })
    }

    fn parse_paren(&mut self, span: Span) -> Result<Expr, CompileError> {
        if self.eat(&TokenKind::RParen) {
            return Err(CompileError::at(
                self.source,
                span.start,
                "expected an expression",
            ));
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
            return Ok(Expr {
                span,
                kind: ExprKind::Tuple(fields),
            });
        }
        self.expect(&TokenKind::RParen, "')'")?;
        Ok(first)
    }

    fn parse_for(&mut self, span: Span) -> Result<Expr, CompileError> {
        if self.is(&TokenKind::LBrace)
            || self.is(&TokenKind::Lt)
            || self.is(&TokenKind::Shl)
            || matches!(&self.current().kind, TokenKind::Ident(name) if name == "once")
        {
            return self.parse_comprehension(span);
        }
        self.expect(&TokenKind::LParen, "'('")?;
        let pat = self.parse_pat()?;
        self.expect(&TokenKind::In, "`in`")?;
        let iter = self.parse_expr()?;
        self.expect(&TokenKind::RParen, "')'")?;
        let body = self.parse_block()?;
        Ok(Expr {
            span: span.to(body.span),
            kind: ExprKind::For {
                pat,
                iter: Box::new(iter),
                body: Box::new(body),
            },
        })
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
            return Ok(Pat::Bool {
                value: token.kind == TokenKind::True,
                span: token.span,
            });
        }
        if let TokenKind::Char(scalar) = &self.tokens[self.index].kind {
            let scalar = *scalar;
            let span = self.bump().span;
            return Ok(Pat::Char { scalar, span });
        }
        let (name, span) = self.expect_ident()?;
        if self.is(&TokenKind::Not) {
            return Err(CompileError::at(
                self.source,
                span.start,
                "macros are not supported in pattern positions",
            ));
        }
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
            return Err(CompileError::at(
                self.source,
                span.start,
                "expected a pattern",
            ));
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
                Pat::Name {
                    name: field.clone(),
                    span: field_span,
                }
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
            span: span.to(body.span),
            kind: ExprKind::While {
                cond: Box::new(cond),
                body: Box::new(body),
            },
        })
    }

    fn binary(&self, op: BinOp, lhs: Expr, rhs: Expr) -> Expr {
        Expr {
            span: lhs.span.to(rhs.span),
            kind: ExprKind::Binary {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            },
        }
    }

    fn unexpected(&self, token: &Token, kind: &TokenKind) -> CompileError {
        let message = match kind {
            TokenKind::Struct => "unexpected `struct`",
            TokenKind::Import => "unexpected `import`",
            TokenKind::Export => "unexpected `export`",
            TokenKind::Extern => "unexpected `extern`",
            TokenKind::Unsafe => "unexpected `unsafe`",
            TokenKind::Eof => "unexpected end of file",
            _ => "unexpected token",
        };
        CompileError::at(self.source, token.span.start, message)
    }

    fn expect_ident(&mut self) -> Result<(String, Span), CompileError> {
        let token = self.bump();
        match token.kind {
            TokenKind::Ident(name) => Ok((name, token.span)),
            _ => Err(CompileError::at(
                self.source,
                token.span.start,
                "expected a name",
            )),
        }
    }

    /// `>` or the first half of `>>`. The second half stays for the next closer.
    fn eat_gt(&mut self) -> bool {
        if self.pending_gt {
            self.pending_gt = false;
            return true;
        }
        if self.eat(&TokenKind::Gt) {
            return true;
        }
        if self.is(&TokenKind::Shr) {
            self.bump();
            self.pending_gt = true;
            return true;
        }
        false
    }

    fn expect_gt(&mut self) -> Result<(), CompileError> {
        if self.eat_gt() {
            Ok(())
        } else {
            let token = self.current();
            Err(CompileError::at(
                self.source,
                token.span.start,
                "expected '>'",
            ))
        }
    }

    fn expect(&mut self, kind: &TokenKind, what: &str) -> Result<Token, CompileError> {
        let token = self.bump();
        if &token.kind == kind {
            Ok(token)
        } else {
            Err(CompileError::at(
                self.source,
                token.span.start,
                format!("expected {what}"),
            ))
        }
    }

    fn eat(&mut self, kind: &TokenKind) -> bool {
        if self.pending_lt {
            if *kind == TokenKind::Lt {
                self.pending_lt = false;
                return true;
            }
            return false;
        }
        if self.pending_gt {
            if *kind == TokenKind::Gt {
                self.pending_gt = false;
                return true;
            }
            return false;
        }
        if self.is(kind) {
            self.index += 1;
            true
        } else {
            false
        }
    }

    fn is(&self, kind: &TokenKind) -> bool {
        if self.pending_lt {
            return *kind == TokenKind::Lt;
        }
        if self.pending_gt {
            return *kind == TokenKind::Gt;
        }
        self.current().kind == *kind
    }

    fn is_ident(&self) -> bool {
        !self.pending_lt && !self.pending_gt && matches!(self.current().kind, TokenKind::Ident(_))
    }

    fn peek_assign(&self) -> bool {
        let Some(token) = self.tokens.get(self.index + 1) else {
            return false;
        };
        Self::assign_kind(&token.kind)
    }

    fn is_assign_op(&self) -> bool {
        Self::assign_kind(&self.current().kind)
    }

    fn assign_kind(kind: &TokenKind) -> bool {
        matches!(
            kind,
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
        if self.pending_lt {
            self.pending_lt = false;
            let previous = self.prev_span();
            return Token {
                kind: TokenKind::Lt,
                span: Span::new(previous.end - 1, previous.end),
            };
        }
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

fn decode_path(chars: &[u16]) -> String {
    String::from_utf16_lossy(chars)
}

pub(crate) fn validate_continuation(source: &str, expr: &Expr) -> Result<(), CompileError> {
    comprehension::validate_expanded(source, expr)
}
