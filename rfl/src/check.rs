use std::collections::HashMap;

use crate::ast::{BinOp, Block, Expr, ExprKind, Function, Module, ParsedTy, Pat, Stmt, UnaryOp};
use crate::error::CompileError;
use crate::ty::Ty;
use crate::typed::*;

struct Sig {
    params: Vec<Ty>,
    ret: Option<Ty>,
}

#[derive(Clone)]
struct Binding {
    ty: Ty,
    mutable: bool,
}

struct AdtSchema {
    name: String,
    params: Vec<String>,
    ctors: Vec<Vec<ParsedTy>>,
}

struct FnSchema {
    index: usize,
    type_params: Vec<String>,
}

struct Suspended {
    scopes: Vec<HashMap<String, u32>>,
    bindings: Vec<Binding>,
    fn_ret: Ty,
    loops: u32,
}

pub fn check(source: &str, module: &Module) -> Result<TModule, CompileError> {
    let mut checker = Checker {
        source,
        sigs: HashMap::new(),
        scopes: Vec::new(),
        bindings: Vec::new(),
        fn_ret: Ty::Unit,
        loops: 0,
        schemas: Vec::new(),
        ctor_schema: HashMap::new(),
        adts: Vec::new(),
        mono: HashMap::new(),
        fn_schemas: HashMap::new(),
        ast_fns: module.functions.clone(),
        fn_mono: HashMap::new(),
        typed: Vec::new(),
        funcs: Vec::new(),
        lambda_count: 0,
        outer_scopes: Vec::new(),
        outer_bindings: Vec::new(),
        captures: Vec::new(),
    };
    checker.register(module)?;
    let concrete: Vec<Function> = module
        .functions
        .iter()
        .filter(|func| func.type_params.is_empty())
        .cloned()
        .collect();
    for func in &concrete {
        checker.check_fn(func, &func.name, true, false)?;
    }
    Ok(TModule { functions: checker.typed })
}

fn is_builtin(name: &str) -> bool {
    matches!(name, "print" | "println" | "putc")
}

fn always_returns(block: &Block) -> bool {
    match block.stmts.last() {
        Some(Stmt::Return { .. }) => true,
        Some(Stmt::Expr(expr)) => expr_returns(expr),
        _ => false,
    }
}

fn expr_returns(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::If { then_body, else_body, .. } => {
            always_returns(then_body) && else_body.as_ref().is_some_and(|e| expr_returns(e))
        }
        ExprKind::Block(block) => block.tail.is_none() && always_returns(block),
        _ => false,
    }
}

struct Checker<'a> {
    source: &'a str,
    sigs: HashMap<String, Sig>,
    scopes: Vec<HashMap<String, u32>>,
    bindings: Vec<Binding>,
    fn_ret: Ty,
    loops: u32,
    schemas: Vec<AdtSchema>,
    /// Constructor name to `(schema index, tag)`.
    ctor_schema: HashMap<String, (usize, u16)>,
    adts: Vec<AdtDef>,
    mono: HashMap<(usize, Vec<Ty>), u32>,
    fn_schemas: HashMap<String, FnSchema>,
    ast_fns: Vec<Function>,
    fn_mono: HashMap<(String, Vec<Ty>), String>,
    typed: Vec<TFn>,
    funcs: Vec<FuncSig>,
    lambda_count: u32,
    /// Bindings of the function that is checking a lambda. Empty otherwise.
    outer_scopes: Vec<HashMap<String, u32>>,
    outer_bindings: Vec<Binding>,
    captures: Vec<crate::typed::Capture>,
}

#[derive(Clone)]
struct FuncSig {
    params: Vec<Ty>,
    ret: Ty,
}

impl<'a> Checker<'a> {
    fn bind(&mut self, name: String, ty: Ty, mutable: bool) -> u32 {
        let id = self.bindings.len() as u32;
        self.bindings.push(Binding { ty, mutable });
        self.scopes.last_mut().unwrap().insert(name, id);
        id
    }

    fn lookup(&self, name: &str) -> Option<u32> {
        for scope in self.scopes.iter().rev() {
            if let Some(id) = scope.get(name) {
                return Some(*id);
            }
        }
        None
    }

    /// A name from an enclosing function becomes an immutable local.
    /// The value is copied when the closure is created.
    fn resolve(&mut self, name: &str) -> Option<u32> {
        if let Some(id) = self.lookup(name) {
            return Some(id);
        }
        let parent = self.outer_scopes.iter().rev().find_map(|scope| scope.get(name).copied())?;
        if let Some(found) = self.captures.iter().find(|cap| cap.parent == parent) {
            self.scopes.last_mut().unwrap().insert(name.to_string(), found.local);
            return Some(found.local);
        }
        let ty = self.outer_bindings[parent as usize].ty;
        let local = self.bind(name.to_string(), ty, false);
        self.captures.push(crate::typed::Capture { parent, local, ty });
        Some(local)
    }

    fn register(&mut self, module: &Module) -> Result<(), CompileError> {
        for adt in &module.adts {
            if self.schemas.iter().any(|schema| schema.name == adt.name) {
                return Err(CompileError::at(self.source, adt.name_span.start, format!("duplicate type `{}`", adt.name)));
            }
            let mut params = Vec::new();
            for param in &adt.params {
                if params.contains(&param.name) {
                    return Err(CompileError::at(self.source, param.span.start, format!("duplicate type parameter `{}`", param.name)));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(self.source, param.span.start, format!("`{}` is not a valid type parameter", param.name)));
                }
                params.push(param.name.clone());
            }
            self.schemas.push(AdtSchema { name: adt.name.clone(), params, ctors: Vec::new() });
        }
        for (index, adt) in module.adts.iter().enumerate() {
            for ctor in &adt.ctors {
                if is_builtin(&ctor.name) {
                    return Err(CompileError::at(self.source, ctor.name_span.start, format!("`{}` is a builtin", ctor.name)));
                }
                if self.ctor_schema.contains_key(&ctor.name) {
                    return Err(CompileError::at(self.source, ctor.name_span.start, format!("duplicate constructor `{}`", ctor.name)));
                }
                for field in &ctor.fields {
                    self.validate_ty(field, &self.schemas[index].params.clone())?;
                }
                let tag = self.schemas[index].ctors.len() as u16;
                self.schemas[index].ctors.push(ctor.fields.clone());
                self.ctor_schema.insert(ctor.name.clone(), (index, tag));
            }
        }
        let plain: Vec<usize> = self
            .schemas
            .iter()
            .enumerate()
            .filter(|(_, schema)| schema.params.is_empty())
            .map(|(index, _)| index)
            .collect();
        for index in plain {
            self.monomorphize(index, &[])?;
        }
        for (index, func) in module.functions.iter().enumerate() {
            if is_builtin(&func.name) {
                return Err(CompileError::at(self.source, func.name_span.start, format!("`{}` is a builtin", func.name)));
            }
            if self.sigs.contains_key(&func.name) || self.fn_schemas.contains_key(&func.name) || self.ctor_schema.contains_key(&func.name) {
                return Err(CompileError::at(self.source, func.name_span.start, format!("duplicate function `{}`", func.name)));
            }
            let mut type_params = Vec::new();
            for param in &func.type_params {
                if type_params.contains(&param.name) {
                    return Err(CompileError::at(self.source, param.span.start, format!("duplicate type parameter `{}`", param.name)));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(self.source, param.span.start, format!("`{}` is not a valid type parameter", param.name)));
                }
                type_params.push(param.name.clone());
            }
            for param in &func.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(self.source, param.name_span.start, format!("parameter `{}` needs a type", param.name)));
                };
                self.validate_ty(ty, &type_params)?;
            }
            if let Some(ret) = &func.ret {
                self.validate_ty(ret, &type_params)?;
            }
            if !type_params.is_empty() {
                if func.name == "main" {
                    return Err(CompileError::at(self.source, func.name_span.start, "main cannot be generic"));
                }
                if func.ret.is_none() {
                    return Err(CompileError::at(self.source, func.name_span.start, format!("generic function `{}` needs a return type", func.name)));
                }
                self.fn_schemas.insert(func.name.clone(), FnSchema { index, type_params });
                continue;
            }
            let mut params = Vec::new();
            for param in &func.params {
                params.push(self.apply(param.ty.as_ref().unwrap(), &HashMap::new())?);
            }
            let ret = match &func.ret {
                Some(ty) => Some(self.apply(ty, &HashMap::new())?),
                None => None,
            };
            self.sigs.insert(func.name.clone(), Sig { params, ret });
        }
        Ok(())
    }

    fn validate_ty(&self, ty: &ParsedTy, params: &[String]) -> Result<(), CompileError> {
        match ty {
            ParsedTy::Func { params: fn_params, ret, .. } => {
                for param in fn_params {
                    self.validate_ty(param, params)?;
                }
                self.validate_ty(ret, params)
            }
            ParsedTy::Named { span, name, args } => {
                if params.iter().any(|param| param == name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(self.source, span.start, format!("type parameter `{name}` does not take arguments")));
                    }
                    return Ok(());
                }
                if Ty::parse_name(name).is_some() {
                    if !args.is_empty() {
                        return Err(CompileError::at(self.source, span.start, format!("`{name}` does not take type arguments")));
                    }
                    return Ok(());
                }
                let Some(schema) = self.schemas.iter().find(|schema| schema.name == *name) else {
                    return Err(CompileError::at(self.source, span.start, format!("unknown type `{name}`")));
                };
                if args.len() != schema.params.len() {
                    return Err(self.arity_error(span.start, name, schema.params.len(), args.len()));
                }
                for arg in args {
                    self.validate_ty(arg, params)?;
                }
                Ok(())
            }
        }
    }

    fn arity_error(&self, offset: usize, name: &str, expect: usize, found: usize) -> CompileError {
        if expect == 0 {
            CompileError::at(self.source, offset, format!("`{name}` does not take type arguments"))
        } else {
            CompileError::at(self.source, offset, format!("`{name}` expects {expect} type arguments, found {found}"))
        }
    }

    fn check_fn(&mut self, func: &Function, name: &str, closure: bool, capture: bool) -> Result<(), CompileError> {
        let saved = Suspended {
            scopes: std::mem::take(&mut self.scopes),
            bindings: std::mem::take(&mut self.bindings),
            fn_ret: self.fn_ret,
            loops: self.loops,
        };
        let saved_outer_scopes = std::mem::take(&mut self.outer_scopes);
        let saved_outer_bindings = std::mem::take(&mut self.outer_bindings);
        let saved_captures = std::mem::take(&mut self.captures);
        if capture {
            self.outer_scopes = saved.scopes.clone();
            self.outer_bindings = saved.bindings.clone();
        }
        self.scopes = vec![HashMap::new()];
        self.loops = 0;
        let declared = self.sigs[name].ret;
        self.fn_ret = declared.unwrap_or(Ty::Unit);
        let mut params = Vec::new();
        let mut seen = HashMap::new();
        for (index, param) in func.params.iter().enumerate() {
            if seen.contains_key(&param.name) {
                return Err(CompileError::at(self.source, param.name_span.start, format!("duplicate parameter `{}`", param.name)));
            }
            seen.insert(param.name.clone(), ());
            let ty = self.sigs[name].params[index];
            params.push(self.bind(param.name.clone(), ty, false));
        }
        let ret = declared.unwrap_or_else(|| self.peek_block(&func.body).unwrap_or(Ty::I16));
        if func.name == "main" && !matches!(ret, Ty::Unit | Ty::I16) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("main returns unit or i16, not {}", self.show(ret)),
            ));
        }
        self.fn_ret = ret;
        self.sigs.get_mut(name).unwrap().ret = Some(ret);
        let body = self.check_block(&func.body, Some(ret))?;
        let body_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
        if func.body.tail.is_none() && ret != Ty::Unit && !always_returns(&func.body) {
            return Err(CompileError::at(self.source, func.name_span.start, format!("`{}` must return {}", func.name, self.show(ret))));
        }
        if func.body.tail.is_some() && body_ty != ret {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("`{}` returns {}, the body has type {}", func.name, self.show(ret), self.show(body_ty)),
            ));
        }
        let bindings = self.bindings.iter().map(|binding| binding.ty).collect();
        let captures = std::mem::take(&mut self.captures);
        self.typed.push(TFn { name: name.to_string(), params, ret, bindings, body, closure, captures });
        self.scopes = saved.scopes;
        self.bindings = saved.bindings;
        self.fn_ret = saved.fn_ret;
        self.loops = saved.loops;
        self.outer_scopes = saved_outer_scopes;
        self.outer_bindings = saved_outer_bindings;
        self.captures = saved_captures;
        Ok(())
    }

    fn instantiate(&mut self, name: &str, args: &[Ty]) -> Result<String, CompileError> {
        let key = (name.to_string(), args.to_vec());
        if let Some(mangled) = self.fn_mono.get(&key) {
            return Ok(mangled.clone());
        }
        let mangled = self.mangle(name, args);
        self.fn_mono.insert(key, mangled.clone());
        let index = self.fn_schemas[name].index;
        let type_params = self.fn_schemas[name].type_params.clone();
        let func = self.ast_fns[index].clone();
        let mut subst = HashMap::new();
        for (param, arg) in type_params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        let mut params = Vec::new();
        for param in &func.params {
            params.push(self.apply(param.ty.as_ref().unwrap(), &subst)?);
        }
        let ret = self.apply(func.ret.as_ref().unwrap(), &subst)?;
        self.sigs.insert(mangled.clone(), Sig { params, ret: Some(ret) });
        self.check_fn(&func, &mangled, true, false)?;
        Ok(mangled)
    }

    fn mangle(&self, name: &str, args: &[Ty]) -> String {
        let mut mangled = name.to_string();
        for arg in args {
            mangled.push('$');
            mangled.push_str(&self.mangle_ty(*arg));
        }
        mangled
    }

    fn mangle_ty(&self, ty: Ty) -> String {
        match ty {
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                let mut text = self.schemas[adt.schema].name.clone();
                for arg in &adt.args {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*arg));
                }
                text.push('$');
                text
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let mut text = String::from("fn");
                for param in &sig.params {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*param));
                }
                text.push('$');
                text.push_str(&self.mangle_ty(sig.ret));
                text.push('$');
                text
            }
            other => other.to_string(),
        }
    }

    fn monomorphize(&mut self, schema: usize, args: &[Ty]) -> Result<u32, CompileError> {
        let key = (schema, args.to_vec());
        if let Some(id) = self.mono.get(&key) {
            return Ok(*id);
        }
        let name = self.schema_name(schema);
        let params = self.schemas[schema].params.clone();
        let ctors = self.schemas[schema].ctors.clone();
        let id = self.adts.len() as u32;
        let display = applied_name(&name, args, self);
        self.adts.push(AdtDef { name: display, schema, args: args.to_vec(), ctors: Vec::new() });
        self.mono.insert(key, id);
        let mut subst = HashMap::new();
        for (param, arg) in params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        for ctor in ctors {
            let mut fields = Vec::new();
            for field in &ctor {
                fields.push(self.apply(field, &subst)?);
            }
            self.adts[id as usize].ctors.push(CtorDef { fields });
        }
        Ok(id)
    }

    fn schema_name(&self, schema: usize) -> String {
        self.schemas[schema].name.clone()
    }

    fn apply(&mut self, ty: &ParsedTy, subst: &HashMap<String, Ty>) -> Result<Ty, CompileError> {
        match ty {
            ParsedTy::Func { params, ret, .. } => {
                let mut fn_params = Vec::new();
                for param in params {
                    fn_params.push(self.apply(param, subst)?);
                }
                let fn_ret = self.apply(ret, subst)?;
                Ok(self.intern_func(fn_params, fn_ret))
            }
            ParsedTy::Named { span, name, args } => {
                if let Some(found) = subst.get(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(self.source, span.start, format!("type parameter `{name}` does not take arguments")));
                    }
                    return Ok(*found);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(self.source, span.start, format!("`{name}` does not take type arguments")));
                    }
                    return Ok(prim);
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name) else {
                    return Err(CompileError::at(self.source, span.start, format!("unknown type `{name}`")));
                };
                let expect = self.schemas[schema].params.len();
                if args.len() != expect {
                    return Err(self.arity_error(span.start, name, expect, args.len()));
                }
                let mut applied = Vec::new();
                for arg in args {
                    applied.push(self.apply(arg, subst)?);
                }
                let id = self.monomorphize(schema, &applied)?;
                Ok(Ty::Adt(id))
            }
        }
    }

    fn intern_func(&mut self, params: Vec<Ty>, ret: Ty) -> Ty {
        if let Some(index) = self.funcs.iter().position(|sig| sig.params == params && sig.ret == ret) {
            return Ty::Func(index as u32);
        }
        let id = self.funcs.len() as u32;
        self.funcs.push(FuncSig { params, ret });
        Ty::Func(id)
    }

    fn check_block(&mut self, block: &Block, expected: Option<Ty>) -> Result<TBlock, CompileError> {
        self.scopes.push(HashMap::new());
        let mut stmts = Vec::new();
        for stmt in &block.stmts {
            stmts.push(self.check_stmt(stmt)?);
        }
        let tail = match &block.tail {
            Some(expr) => Some(self.check_expr(expr, expected)?),
            None => None,
        };
        self.scopes.pop();
        Ok(TBlock { stmts, tail })
    }

    fn check_stmt(&mut self, stmt: &Stmt) -> Result<TStmt, CompileError> {
        match stmt {
            Stmt::Let { name, name_span, mutable, ty, init } => {
                let annotated = match ty {
                    Some(parsed) => Some(self.apply(parsed, &HashMap::new())?),
                    None => None,
                };
                let init = self.check_expr(init, annotated)?;
                if let Some(want) = annotated {
                    if init.ty != want {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("`{name}` has type {want}, the initializer has type {}", init.ty),
                        ));
                    }
                }
                let id = self.bind(name.clone(), init.ty, *mutable);
                Ok(TStmt::Let { binding: id, init })
            }
            Stmt::Assign { name, name_span, value } => {
                let Some(id) = self.resolve(name) else {
                    return Err(CompileError::at(self.source, name_span.start, format!("unknown name `{name}`")));
                };
                if !self.bindings[id as usize].mutable {
                    let captured = self.captures.iter().any(|cap| cap.local == id);
                    let message = if captured {
                        format!("`{name}` was captured by value and cannot be assigned")
                    } else {
                        format!("`{name}` is not mutable")
                    };
                    return Err(CompileError::at(self.source, name_span.start, message));
                }
                let ty = self.bindings[id as usize].ty;
                let value = self.check_expr(value, Some(ty))?;
                if value.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("cannot assign {} to `{name}` of type {ty}", value.ty),
                    ));
                }
                Ok(TStmt::Assign { binding: id, value })
            }
            Stmt::Expr(expr) => Ok(TStmt::Expr(self.check_expr(expr, None)?)),
            Stmt::Return { span, value } => {
                let value = match value {
                    Some(expr) => Some(self.check_expr(expr, Some(self.fn_ret))?),
                    None => None,
                };
                let got = value.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                if got != self.fn_ret {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("return has type {got}, the function returns {}", self.fn_ret),
                    ));
                }
                Ok(TStmt::Return(value))
            }
            Stmt::Break { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(self.source, span.start, "`break` is outside a loop"));
                }
                Ok(TStmt::Break)
            }
            Stmt::Continue { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(self.source, span.start, "`continue` is outside a loop"));
                }
                Ok(TStmt::Continue)
            }
        }
    }

    fn check_expr(&mut self, expr: &Expr, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let span = expr.span.start;
        match &expr.kind {
            ExprKind::Int(value) => {
                let ty = match expected {
                    Some(ty) if ty.is_int() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(self.source, span, format!("expected {ty}, found an integer literal")));
                    }
                    None => Ty::I16,
                };
                if !ty.in_range(*value) {
                    return Err(CompileError::at(self.source, span, format!("{value} does not fit in {ty}")));
                }
                Ok(TExpr { ty, kind: TExprKind::Int(*value) })
            }
            ExprKind::Bool(value) => {
                if let Some(ty) = expected {
                    if ty != Ty::Bool {
                        return Err(CompileError::at(self.source, span, format!("expected {ty}, found bool")));
                    }
                }
                Ok(TExpr { ty: Ty::Bool, kind: TExprKind::Bool(*value) })
            }
            ExprKind::Name(name) => {
                let Some(id) = self.resolve(name) else {
                    if let Some(value) = self.fn_value(span, name, expected)? {
                        return Ok(value);
                    }
                    return self.nullary(span, name, expected);
                };
                let ty = self.bindings[id as usize].ty;
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(self.source, span, format!("expected {want}, `{name}` has type {ty}")));
                    }
                }
                Ok(TExpr { ty, kind: TExprKind::Name(id) })
            }
            ExprKind::Unary { op, expr: inner } => self.check_unary(span, *op, inner, expected),
            ExprKind::Binary { op, lhs, rhs } => self.check_binary(span, *op, lhs, rhs, expected),
            ExprKind::Str(_) => {
                Err(CompileError::at(self.source, span, "string values are not implemented yet"))
            }
            ExprKind::Call { callee, type_args, args } => self.check_call(span, callee, type_args, args, expected),
            ExprKind::Method { receiver, method, args } => {
                let mut all = vec![(**receiver).clone()];
                all.extend(args.iter().cloned());
                self.check_call(span, method, &[], &all, expected)
            }
            ExprKind::Lambda { params, body } => self.check_lambda(span, params, body, expected),
            ExprKind::Match { scrutinee, arms } => self.check_match(span, scrutinee, arms, expected),
            ExprKind::If { cond, then_body, else_body } => self.check_if(cond, then_body, else_body, expected),
            ExprKind::While { cond, body } => self.check_while(span, cond, body, expected),
            ExprKind::Block(block) => {
                let body = self.check_block(block, expected)?;
                let ty = body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                Ok(TExpr { ty, kind: TExprKind::Block(Box::new(body)) })
            }
            ExprKind::Cast { expr: inner, ty } => {
                let to = self.apply(ty, &HashMap::new())?;
                if !to.is_int() {
                    return Err(CompileError::at(self.source, ty.span().start, format!("cannot cast to {to}")));
                }
                let inner = self.check_expr(inner, None)?;
                if !inner.ty.is_int() {
                    return Err(CompileError::at(self.source, span, format!("cannot cast {} to {to}", inner.ty)));
                }
                if let TExprKind::Int(value) = inner.kind {
                    if !to.in_range(value) {
                        return Err(CompileError::at(self.source, span, format!("{value} does not fit in {to}")));
                    }
                    return Ok(TExpr { ty: to, kind: TExprKind::Int(value) });
                }
                Ok(TExpr { ty: to, kind: TExprKind::Cast { expr: Box::new(inner), to } })
            }
        }
    }

    fn check_unary(&mut self, span: usize, op: UnaryOp, inner: &Expr, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        match op {
            UnaryOp::Not => {
                let inner = self.check_expr(inner, Some(Ty::Bool))?;
                if inner.ty != Ty::Bool {
                    return Err(CompileError::at(self.source, span, format!("`!` expects bool, found {}", inner.ty)));
                }
                Ok(TExpr { ty: Ty::Bool, kind: TExprKind::Unary { op, expr: Box::new(inner) } })
            }
            UnaryOp::Neg => {
                let want = expected.filter(|ty| ty.is_int());
                let inner = self.check_expr(inner, want)?;
                if !inner.ty.is_int() || !inner.ty.is_signed() {
                    return Err(CompileError::at(self.source, span, format!("`-` expects a signed integer, found {}", inner.ty)));
                }
                if let TExprKind::Int(value) = inner.kind {
                    let Some(negated) = value.checked_neg() else {
                        return Err(CompileError::at(self.source, span, format!("-{value} does not fit in {}", inner.ty)));
                    };
                    if !inner.ty.in_range(negated) {
                        return Err(CompileError::at(self.source, span, format!("{negated} does not fit in {}", inner.ty)));
                    }
                    return Ok(TExpr { ty: inner.ty, kind: TExprKind::Int(negated) });
                }
                Ok(TExpr { ty: inner.ty, kind: TExprKind::Unary { op, expr: Box::new(inner) } })
            }
        }
    }

    fn check_binary(&mut self, span: usize, op: BinOp, lhs: &Expr, rhs: &Expr, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        if op.is_logic() {
            let lhs = self.check_expr(lhs, Some(Ty::Bool))?;
            let rhs = self.check_expr(rhs, Some(Ty::Bool))?;
            if lhs.ty != Ty::Bool || rhs.ty != Ty::Bool {
                return Err(CompileError::at(self.source, span, "`&&` and `||` expect bool"));
            }
            return Ok(TExpr { ty: Ty::Bool, kind: TExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) } });
        }
        let operand_ty = if op.is_compare() {
            self.peek(lhs).or_else(|| self.peek(rhs)).unwrap_or(Ty::I16)
        } else {
            expected.filter(|ty| ty.is_int())
                .or_else(|| self.peek(lhs))
                .or_else(|| self.peek(rhs))
                .unwrap_or(Ty::I16)
        };
        if !operand_ty.is_int() {
            return Err(CompileError::at(self.source, span, format!("operator expects integers, found {operand_ty}")));
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
        let ty = if op.is_compare() { Ty::Bool } else { operand_ty };
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(self.source, span, format!("expected {want}, this expression has type {ty}")));
            }
        }
        Ok(TExpr { ty, kind: TExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) } })
    }

    fn check_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if is_builtin(callee) {
            if !type_args.is_empty() {
                return Err(CompileError::at(self.source, span, format!("`{callee}` does not take type arguments")));
            }
            return self.check_builtin(span, callee, args, expected);
        }
        if let Some(id) = self.resolve(callee) {
            return self.check_apply(span, callee, id, type_args, args, expected);
        }
        if self.ctor_schema.contains_key(callee) {
            return self.check_ctor(span, callee, type_args, args, expected);
        }
        if self.fn_schemas.contains_key(callee) {
            return self.check_generic_call(span, callee, type_args, args, expected);
        }
        if !type_args.is_empty() {
            return Err(CompileError::at(self.source, span, format!("`{callee}` does not take type arguments")));
        }
        let Some(sig) = self.sigs.get(callee) else {
            return Err(CompileError::at(self.source, span, format!("unknown function `{callee}`")));
        };
        let Some(ret) = sig.ret else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("function `{callee}` needs a return type before it can be called"),
            ));
        };
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {} arguments, found {}", sig.params.len(), args.len()),
            ));
        }
        let params = sig.params.clone();
        let mut checked = Vec::new();
        for (arg, ty) in args.iter().zip(params) {
            let arg = self.check_expr(arg, Some(ty))?;
            if arg.ty != ty {
                return Err(CompileError::at(self.source, span, format!("expected {}, found {}", self.show(ty), self.show(arg.ty))));
            }
            checked.push(arg);
        }
        Ok(TExpr { ty: ret, kind: TExprKind::Call { callee: callee.to_string(), args: checked } })
    }

    fn check_apply(
        &mut self,
        span: usize,
        callee: &str,
        binding: u32,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ty = self.bindings[binding as usize].ty;
        let Ty::Func(id) = ty else {
            return Err(CompileError::at(self.source, span, format!("`{callee}` is not a function")));
        };
        if !type_args.is_empty() {
            return Err(CompileError::at(self.source, span, format!("`{callee}` does not take type arguments")));
        }
        let sig = self.funcs[id as usize].clone();
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {} arguments, found {}", sig.params.len(), args.len()),
            ));
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&sig.params) {
            let typed = self.check_expr(arg, Some(*param))?;
            if typed.ty != *param {
                return Err(CompileError::at(self.source, span, format!("expected {}, found {}", self.show(*param), self.show(typed.ty))));
            }
            checked.push(typed);
        }
        if let Some(want) = expected {
            if want != sig.ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `{callee}` has type {}", self.show(want), self.show(sig.ret)),
                ));
            }
        }
        Ok(TExpr {
            ty: sig.ret,
            kind: TExprKind::Apply {
                callee: Box::new(TExpr { ty, kind: TExprKind::Name(binding) }),
                args: checked,
                params: sig.params,
            },
        })
    }

    fn check_generic_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let type_params = self.fn_schemas[callee].type_params.clone();
        let func = self.ast_fns[self.fn_schemas[callee].index].clone();
        if args.len() != func.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {} arguments, found {}", func.params.len(), args.len()),
            ));
        }
        let mut solved = vec![None; type_params.len()];
        if !type_args.is_empty() {
            if type_args.len() != type_params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {} type arguments, found {}", type_params.len(), type_args.len()),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                solved[index] = Some(self.apply(ty, &HashMap::new())?);
            }
        } else if let Some(want) = expected {
            if let Some(ret) = &func.ret {
                self.infer_field(ret, want, &type_params, &mut solved, span)?;
            }
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&func.params) {
            let param_ty = param.ty.as_ref().unwrap();
            if let Some(ty) = self.concrete_field(param_ty, &type_params, &solved)? {
                let typed = self.check_expr(arg, Some(ty))?;
                if typed.ty != ty {
                    return Err(CompileError::at(self.source, span, format!("expected {}, found {}", self.show(ty), self.show(typed.ty))));
                }
                checked.push(typed);
            } else {
                let typed = self.check_expr(arg, None)?;
                self.infer_field(param_ty, typed.ty, &type_params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(self.source, span, format!("cannot infer type arguments for `{callee}`")));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let mangled = self.instantiate(callee, &concrete)?;
        let ret = self.sigs[&mangled].ret.unwrap();
        if let Some(want) = expected {
            if want != ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `{callee}` has type {}", self.show(want), self.show(ret)),
                ));
            }
        }
        Ok(TExpr { ty: ret, kind: TExprKind::Call { callee: mangled, args: checked } })
    }

    /// `print`, `println`, and `putc` are temporary compiler builtins so programs
    /// can log values before `std/io` exists. They lower to direct `putchar` calls.
    fn check_builtin(&mut self, span: usize, callee: &str, args: &[Expr], expected: Option<Ty>) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(self.source, span, format!("expected {want}, `{callee}` has type unit")));
            }
        }
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects 1 argument, found {}", args.len()),
            ));
        }
        if callee == "putc" {
            let arg = self.check_expr(&args[0], None)?;
            if !arg.ty.is_int() {
                return Err(CompileError::at(self.source, span, format!("`putc` expects an integer, found {}", self.show(arg.ty))));
            }
            return Ok(TExpr { ty: Ty::Unit, kind: TExprKind::Putc(Box::new(arg)) });
        }
        let newline = callee == "println";
        if let ExprKind::Str(chars) = &args[0].kind {
            return Ok(TExpr { ty: Ty::Unit, kind: TExprKind::PrintLit { chars: chars.clone(), newline } });
        }
        let arg = self.check_expr(&args[0], None)?;
        if !arg.ty.is_int() && arg.ty != Ty::Bool {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects an integer, bool, or string, found {}", self.show(arg.ty)),
            ));
        }
        Ok(TExpr { ty: Ty::Unit, kind: TExprKind::Print { value: Box::new(arg), newline } })
    }

    fn check_if(&mut self, cond: &Expr, then_body: &Block, else_body: &Option<Box<Expr>>, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        let Some(else_body) = else_body else {
            let then_body = self.check_block(then_body, None)?;
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::If { cond: Box::new(cond), then_body: Box::new(then_body), else_body: None },
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
            kind: TExprKind::If { cond: Box::new(cond), then_body: Box::new(then_body), else_body: Some(Box::new(else_expr)) },
        })
    }

    fn check_while(&mut self, span: usize, cond: &Expr, body: &Block, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(self.source, span, format!("expected {want}, `while` has type unit")));
            }
        }
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        if cond.ty != Ty::Bool {
            return Err(CompileError::at(self.source, span, format!("`while` expects bool, found {}", cond.ty)));
        }
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        Ok(TExpr { ty: Ty::Unit, kind: TExprKind::While { cond: Box::new(cond), body: Box::new(body) } })
    }

    /// A monomorphic function name is a callable value: code id plus an empty environment.
    /// Every function frame reserves the environment slot, so the same call protocol reaches
    /// named functions and lambdas.
    fn fn_value(&mut self, span: usize, name: &str, expected: Option<Ty>) -> Result<Option<TExpr>, CompileError> {
        if self.fn_schemas.contains_key(name) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("generic function `{name}` needs a concrete signature to be used as a value"),
            ));
        }
        let Some(sig) = self.sigs.get(name) else {
            return Ok(None);
        };
        let Some(ret) = sig.ret else {
            return Err(CompileError::at(self.source, span, format!("function `{name}` needs a return type to be used as a value")));
        };
        let params = sig.params.clone();
        let ty = self.intern_func(params, ret);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `{name}` has type {}", self.show(want), self.show(ty)),
                ));
            }
        }
        Ok(Some(TExpr { ty, kind: TExprKind::Closure { func: name.to_string() } }))
    }

    fn show(&self, ty: Ty) -> String {
        match ty {
            Ty::Adt(id) => self.adts[id as usize].name.clone(),
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let params: Vec<String> = sig.params.iter().map(|param| self.show(*param)).collect();
                format!("fn({}) -> {}", params.join(", "), self.show(sig.ret))
            }
            other => other.to_string(),
        }
    }

    fn check_lambda(&mut self, span: usize, params: &[crate::ast::Param], body: &Expr, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let expected_sig = match expected {
            Some(Ty::Func(id)) => Some(self.funcs[id as usize].clone()),
            Some(want) => {
                return Err(CompileError::at(self.source, span, format!("expected {}, found a function", self.show(want))));
            }
            None => None,
        };
        if let Some(sig) = &expected_sig {
            if sig.params.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {} parameters, found {}", sig.params.len(), params.len()),
                ));
            }
        }
        let mut sig_params = Vec::new();
        for (index, param) in params.iter().enumerate() {
            let ty = if let Some(parsed) = &param.ty {
                let ty = self.apply(parsed, &HashMap::new())?;
                if let Some(sig) = &expected_sig {
                    if sig.params[index] != ty {
                        return Err(CompileError::at(
                            self.source,
                            param.name_span.start,
                            format!("expected {}, `{}` has type {}", self.show(sig.params[index]), param.name, self.show(ty)),
                        ));
                    }
                }
                ty
            } else if let Some(sig) = &expected_sig {
                sig.params[index]
            } else {
                Ty::I16
            };
            sig_params.push(ty);
        }
        let name = format!("__lambda{}", self.lambda_count);
        self.lambda_count += 1;
        let declared_ret = expected_sig.as_ref().map(|sig| sig.ret);
        self.sigs.insert(name.clone(), Sig { params: sig_params.clone(), ret: declared_ret });
        let block = match &body.kind {
            ExprKind::Block(block) => (**block).clone(),
            _ => Block { stmts: Vec::new(), tail: Some(body.clone()) },
        };
        let func = Function {
            name: name.clone(),
            name_span: crate::ast::Span { start: span },
            type_params: Vec::new(),
            params: params.to_vec(),
            ret: None,
            body: block,
        };
        self.check_fn(&func, &name, true, true)?;
        let ret = self.sigs[&name].ret.unwrap();
        let ty = self.intern_func(sig_params, ret);
        Ok(TExpr { ty, kind: TExprKind::Closure { func: name } })
    }

    fn nullary(&self, span: usize, name: &str, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(self.source, span, format!("unknown name `{name}`")));
        };
        if !self.schemas[schema].ctors[tag as usize].is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` expects {} arguments", self.schemas[schema].ctors[tag as usize].len()),
            ));
        }
        let id = if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema != schema {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `{name}` constructs {}", self.show(Ty::Adt(id)), self.schemas[schema].name),
                ));
            }
            id
        } else if self.schemas[schema].params.is_empty() {
            let id = self.mono[&(schema, Vec::new())];
            if let Some(want) = expected {
                if want != Ty::Adt(id) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `{name}` has type {}", self.show(want), self.show(Ty::Adt(id))),
                    ));
                }
            }
            id
        } else if let Some(want) = expected {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected {}, `{name}` constructs {}", self.show(want), self.schemas[schema].name),
            ));
        } else {
            return Err(CompileError::at(self.source, span, format!("cannot infer type arguments for `{name}`")));
        };
        Ok(TExpr { ty: Ty::Adt(id), kind: TExprKind::Ctor { tag, args: Vec::new() } })
    }

    fn check_ctor(
        &mut self,
        span: usize,
        name: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let (schema, tag) = self.ctor_schema[name];
        let params = self.schemas[schema].params.clone();
        let fields = self.schemas[schema].ctors[tag as usize].clone();
        if !type_args.is_empty() && params.is_empty() {
            return Err(CompileError::at(self.source, span, format!("`{name}` does not take type arguments")));
        }
        if args.len() != fields.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` expects {} arguments, found {}", fields.len(), args.len()),
            ));
        }
        let mut solved = vec![None; params.len()];
        if !type_args.is_empty() {
            if type_args.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{name}` expects {} type arguments, found {}", params.len(), type_args.len()),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                solved[index] = Some(self.apply(ty, &HashMap::new())?);
            }
        } else if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema == schema {
                for (index, arg) in self.adts[id as usize].args.iter().enumerate() {
                    solved[index] = Some(*arg);
                }
            }
        }
        let mut checked = Vec::new();
        for (arg, field) in args.iter().zip(&fields) {
            if let Some(ty) = self.concrete_field(field, &params, &solved)? {
                let typed = self.check_expr(arg, Some(ty))?;
                if typed.ty != ty {
                    return Err(CompileError::at(self.source, span, format!("expected {}, found {}", self.show(ty), self.show(typed.ty))));
                }
                checked.push(typed);
            } else {
                let typed = self.check_expr(arg, None)?;
                self.infer_field(field, typed.ty, &params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(self.source, span, format!("cannot infer type arguments for `{name}`")));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let id = self.monomorphize(schema, &concrete)?;
        let ty = Ty::Adt(id);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(self.source, span, format!("expected {}, `{name}` has type {}", self.show(want), self.show(ty))));
            }
        }
        Ok(TExpr { ty, kind: TExprKind::Ctor { tag, args: checked } })
    }

    fn concrete_field(&mut self, field: &ParsedTy, params: &[String], solved: &[Option<Ty>]) -> Result<Option<Ty>, CompileError> {
        if mentions_unsolved(field, params, solved) {
            return Ok(None);
        }
        let mut subst = HashMap::new();
        for (index, param) in params.iter().enumerate() {
            if let Some(ty) = solved[index] {
                subst.insert(param.clone(), ty);
            }
        }
        Ok(Some(self.apply(field, &subst)?))
    }

    fn infer_field(
        &self,
        parsed: &ParsedTy,
        actual: Ty,
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
    ) -> Result<(), CompileError> {
        match parsed {
            ParsedTy::Func { params: fn_params, ret, .. } => {
                let Ty::Func(id) = actual else {
                    return Err(CompileError::at(self.source, span, format!("expected a function, found {}", self.show(actual))));
                };
                let sig = self.funcs[id as usize].clone();
                if fn_params.len() != sig.params.len() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {} parameters, found {}", fn_params.len(), sig.params.len()),
                    ));
                }
                for (param, got) in fn_params.iter().zip(sig.params) {
                    self.infer_field(param, got, params, solved, span)?;
                }
                self.infer_field(ret, sig.ret, params, solved, span)
            }
            ParsedTy::Named { span: name_span, name, args } => {
                if let Some(index) = params.iter().position(|param| param == name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(self.source, name_span.start, format!("type parameter `{name}` does not take arguments")));
                    }
                    match solved[index] {
                        None => solved[index] = Some(actual),
                        Some(existing) if existing == actual => {}
                        Some(existing) => {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected {}, found {}", self.show(existing), self.show(actual)),
                            ));
                        }
                    }
                    return Ok(());
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if prim != actual {
                        return Err(CompileError::at(self.source, span, format!("expected {prim}, found {}", self.show(actual))));
                    }
                    return Ok(());
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name) else {
                    return Err(CompileError::at(self.source, name_span.start, format!("unknown type `{name}`")));
                };
                let Ty::Adt(id) = actual else {
                    return Err(CompileError::at(self.source, span, format!("expected {name}, found {}", self.show(actual))));
                };
                if self.adts[id as usize].schema != schema {
                    return Err(CompileError::at(self.source, span, format!("expected {name}, found {}", self.show(actual))));
                }
                let concrete = self.adts[id as usize].args.clone();
                if args.len() != concrete.len() {
                    return Err(self.arity_error(name_span.start, name, concrete.len(), args.len()));
                }
                for (arg, got) in args.iter().zip(concrete) {
                    self.infer_field(arg, got, params, solved, span)?;
                }
                Ok(())
            }
        }
    }

    fn check_match(&mut self, span: usize, scrutinee: &Expr, arms: &[crate::ast::Arm], expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let scrutinee = self.check_expr(scrutinee, None)?;
        let Ty::Adt(adt) = scrutinee.ty else {
            return Err(CompileError::at(self.source, span, format!("match expects a data type, found {}", self.show(scrutinee.ty))));
        };
        let mut covered = vec![false; self.adts[adt as usize].ctors.len()];
        let mut seen_total = false;
        let mut result: Option<Ty> = expected;
        let mut typed = Vec::new();
        for arm in arms {
            if seen_total || covered.iter().all(|hit| *hit) {
                return Err(CompileError::at(self.source, arm_span(&arm.pat), "unreachable pattern"));
            }
            self.scopes.push(HashMap::new());
            let pat = self.check_pat(&arm.pat, scrutinee.ty)?;
            if let TPat::Ctor { tag, .. } = &pat {
                if covered[*tag as usize] {
                    return Err(CompileError::at(self.source, arm_span(&arm.pat), "unreachable pattern"));
                }
            }
            match &pat {
                TPat::Wild | TPat::Bind(_) => seen_total = true,
                TPat::Ctor { tag, fields } if fields.iter().all(|field| irrefutable(&field.pat)) => {
                    covered[*tag as usize] = true;
                }
                TPat::Ctor { .. } => {}
            }
            let body = match &arm.body.kind {
                ExprKind::Block(block) => self.check_block(block, result)?,
                _ => {
                    let expr = self.check_expr(&arm.body, result)?;
                    TBlock { stmts: Vec::new(), tail: Some(expr) }
                }
            };
            self.scopes.pop();
            let arm_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
            if let Some(want) = result {
                if arm_ty != want {
                    return Err(CompileError::at(
                        self.source,
                        arm.body.span.start,
                        format!("expected {}, this arm has type {}", self.show(want), self.show(arm_ty)),
                    ));
                }
            }
            result = Some(arm_ty);
            typed.push(TArm { pat, body });
        }
        if !seen_total && covered.iter().any(|hit| !hit) {
            return Err(CompileError::at(self.source, span, "non-exhaustive match"));
        }
        let ty = result.unwrap_or(Ty::Unit);
        Ok(TExpr { ty, kind: TExprKind::Match { scrutinee: Box::new(scrutinee), arms: typed } })
    }

    fn check_pat(&mut self, pat: &Pat, ty: Ty) -> Result<TPat, CompileError> {
        match pat {
            Pat::Name { name, .. } if name == "_" => Ok(TPat::Wild),
            Pat::Name { name, span } => {
                if self.ctor_schema.contains_key(name) {
                    let (tag, fields) = self.ctor_fields(name, ty, span.start)?;
                    if !fields.is_empty() {
                        return Err(CompileError::at(self.source, span.start, format!("`{name}` expects {} fields", fields.len())));
                    }
                    return Ok(TPat::Ctor { tag, fields: Vec::new() });
                }
                let id = self.bind(name.clone(), ty, false);
                Ok(TPat::Bind(id))
            }
            Pat::Ctor { name, span, fields: pats } => {
                let (tag, field_tys) = self.ctor_fields(name, ty, span.start)?;
                if pats.len() != field_tys.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("`{name}` expects {} fields, found {}", field_tys.len(), pats.len()),
                    ));
                }
                let mut fields = Vec::new();
                for (pat, field_ty) in pats.iter().zip(field_tys) {
                    fields.push(FieldPat { ty: field_ty, pat: Box::new(self.check_pat(pat, field_ty)?) });
                }
                Ok(TPat::Ctor { tag, fields })
            }
        }
    }

    fn ctor_fields(&self, name: &str, ty: Ty, span: usize) -> Result<(u16, Vec<Ty>), CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(self.source, span, format!("unknown constructor `{name}`")));
        };
        let Ty::Adt(id) = ty else {
            return Err(CompileError::at(self.source, span, format!("`{name}` is not a constructor of {}", self.show(ty))));
        };
        if self.adts[id as usize].schema != schema {
            return Err(CompileError::at(self.source, span, format!("`{name}` is not a constructor of {}", self.show(ty))));
        }
        Ok((tag, self.adts[id as usize].ctors[tag as usize].fields.clone()))
    }
}

fn mentions_unsolved(ty: &ParsedTy, params: &[String], solved: &[Option<Ty>]) -> bool {
    match ty {
        ParsedTy::Func { params: fn_params, ret, .. } => {
            fn_params.iter().any(|param| mentions_unsolved(param, params, solved)) || mentions_unsolved(ret, params, solved)
        }
        ParsedTy::Named { name, args, .. } => {
            if let Some(index) = params.iter().position(|param| param == name) {
                return solved[index].is_none();
            }
            args.iter().any(|arg| mentions_unsolved(arg, params, solved))
        }
    }
}

fn applied_name(name: &str, args: &[Ty], checker: &Checker) -> String {
    if args.is_empty() {
        return name.to_string();
    }
    let parts: Vec<String> = args.iter().map(|ty| checker.show(*ty)).collect();
    format!("{name}<{}>", parts.join(", "))
}

fn irrefutable(pat: &TPat) -> bool {
    matches!(pat, TPat::Wild | TPat::Bind(_))
}

fn arm_span(pat: &Pat) -> usize {
    match pat {
        Pat::Name { span, .. } | Pat::Ctor { span, .. } => span.start,
    }
}

impl<'a> Checker<'a> {
    fn peek(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Int(_) => None,
            ExprKind::Bool(_) => Some(Ty::Bool),
            ExprKind::Name(name) => self.lookup(name).map(|id| self.bindings[id as usize].ty).or_else(|| {
                let id = self.outer_scopes.iter().rev().find_map(|scope| scope.get(name).copied())?;
                Some(self.outer_bindings[id as usize].ty)
            }),
            ExprKind::Unary { op: UnaryOp::Not, .. } => Some(Ty::Bool),
            ExprKind::Unary { op: UnaryOp::Neg, expr } => self.peek(expr),
            ExprKind::Binary { op, .. } if op.is_compare() || op.is_logic() => Some(Ty::Bool),
            ExprKind::Binary { lhs, rhs, .. } => self.peek(lhs).or_else(|| self.peek(rhs)),
            ExprKind::Str(_) | ExprKind::Lambda { .. } => None,
            ExprKind::Call { callee, .. } if is_builtin(callee) => Some(Ty::Unit),
            ExprKind::Call { callee, .. } => self.sigs.get(callee).and_then(|sig| sig.ret),
            ExprKind::Method { method, .. } => self.sigs.get(method).and_then(|sig| sig.ret),
            ExprKind::If { else_body: None, .. } | ExprKind::While { .. } => Some(Ty::Unit),
            ExprKind::Match { arms, .. } => arms.first().and_then(|arm| self.peek(&arm.body)),
            ExprKind::If { then_body, else_body: Some(else_body), .. } => {
                self.peek_block(then_body).or_else(|| self.peek(else_body))
            }
            ExprKind::Block(block) => self.peek_block(block),
            ExprKind::Cast { ty, .. } => match ty {
                ParsedTy::Named { name, args, .. } if args.is_empty() => Ty::parse_name(name),
                _ => None,
            },
        }
    }

    fn peek_block(&self, block: &Block) -> Option<Ty> {
        match &block.tail {
            Some(expr) => self.peek(expr),
            None => Some(Ty::Unit),
        }
    }
}
