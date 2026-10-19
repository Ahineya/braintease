//! Functions in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_fn(
        &mut self,
        func: &Function,
        name: &str,
        closure: bool,
        capture: bool,
    ) -> Result<(), CompileError> {
        let saved_async_context =
            std::mem::replace(&mut self.async_context, func.is_async.then_some(None));
        let saved_assoc_self = self.assoc_self.clone();
        if !capture {
            self.assoc_self = if let Some((tr, owner)) = self.impl_contexts.get(&func.name).cloned()
            {
                let arity = self.traits.iter().find(|t| t.name == tr).unwrap().arity;
                let ty = self.apply_argument(&owner, arity, &HashMap::new())?;
                Some((tr, ty))
            } else {
                None
            };
        }
        let saved = Suspended {
            scopes: std::mem::take(&mut self.scopes),
            bindings: std::mem::take(&mut self.bindings),
            fn_ret: self.fn_ret,
            loops: self.loops,
        };
        let saved_outer_scopes = std::mem::take(&mut self.outer_scopes);
        let saved_outer_bindings = std::mem::take(&mut self.outer_bindings);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_shared = std::mem::take(&mut self.shared_spans);
        self.shared_spans = shared_assign_spans(&func.body);
        let saved_borrows = std::mem::take(&mut self.borrows);
        let saved_loose = self.loose_receiver;
        self.loose_receiver = false;
        if capture {
            self.outer_scopes = saved.scopes.clone();
            self.outer_bindings = saved.bindings.clone();
        }
        self.scopes = vec![HashMap::new()];
        self.loops = 0;
        let declared = self.sigs[name].ret;
        let async_result = func.is_async.then_some(declared.unwrap_or(Ty::Unit));
        let declared = if func.is_async {
            let id = match declared {
                Some(Ty::Adt(id))
                    if self.schemas[self.adts[id as usize].schema].name == "Async" =>
                {
                    id
                }
                _ => return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    "Async is reserved for compiler-owned futures in an async function signature",
                )),
            };
            Some(self.adts[id as usize].args[0])
        } else {
            declared
        };
        self.fn_ret = declared.unwrap_or(Ty::Unit);
        let mut params = Vec::new();
        let mut seen = HashMap::new();
        let mut prefix = Vec::new();
        for (index, param) in func.params.iter().enumerate() {
            let ty = self.sigs[name].params[index];
            if func.is_async {
                self.require_vec_elem(param.name_span.start, ty)?;
            }
            let simple = matches!(&param.pat, Pat::Name { name, .. } if name != "_");
            if simple {
                if seen.contains_key(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("duplicate parameter `{}`", param.name),
                    ));
                }
                seen.insert(param.name.clone(), ());
                let id = self.bind(param.name.clone(), ty, false);
                self.bindings[id as usize].def = param.name_span;
                let shown = self.show(ty);
                self.facts.push(BindingFact {
                    name: param.name.clone(),
                    def: param.name_span,
                    ty: shown,
                    scope: func.body.span,
                    visible_from: func.body.span.start,
                    variant: false,
                });
                params.push(id);
            } else {
                let mut names = Vec::new();
                pat_names(&param.pat, &mut names);
                for (bound, span) in &names {
                    if seen.contains_key(bound) {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("duplicate parameter `{bound}`"),
                        ));
                    }
                    seen.insert(bound.clone(), ());
                }
                let hidden = self.bind(format!("__arg{index}"), ty, false);
                params.push(hidden);
                self.scope_spans.push(func.body.span);
                self.next_visible = Some(func.body.span.start);
                let pat = self.check_pat(&param.pat, ty, false);
                self.scope_spans.pop();
                self.next_visible = None;
                let pat = pat?;
                if !self.irrefutable_pat(&pat, ty) {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        "a parameter pattern must be irrefutable",
                    ));
                }
                prefix.push(TStmt::Destructure {
                    pat,
                    init: TExpr {
                        ty,
                        kind: TExprKind::Name(hidden, 0),
                    },
                });
            }
        }
        let hint = self.peek_block(&func.body);
        // An unknown tail (for example a generic call or a match containing
        // one) must infer its result before constraining callback outputs.
        // Preserve known named-function hints, including context needed by `?`.
        // The provisional numeric default is only for recursive references.
        let infer_result = declared.is_none() && (capture || hint.is_none());
        let saved_lambda_return =
            std::mem::replace(&mut self.lambda_return, infer_result.then_some(None));
        let mut ret = declared.unwrap_or_else(|| hint.unwrap_or(Ty::I16));
        if func.is_async && (func.name == "main" || func.c_export) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                "async functions cannot be main or exported through the C ABI",
            ));
        }
        self.fn_ret = ret;
        if escaped_ref(&self.ty_to_parsed(ret)) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                "a function cannot return a reference",
            ));
        }
        self.sigs.get_mut(name).unwrap().ret = Some(async_result.unwrap_or(ret));
        let mut body = self.check_block(&func.body, if infer_result { None } else { Some(ret) })?;
        for (index, stmt) in prefix.into_iter().enumerate() {
            body.stmts.insert(index, stmt);
        }
        let body_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
        let returning_tail = infer_result && func.body.tail.as_ref().is_some_and(expr_returns);
        if infer_result {
            let from_return = self.lambda_return.flatten();
            ret = if body.tail.is_some() && !returning_tail {
                body_ty
            } else {
                from_return.unwrap_or(Ty::Unit)
            };
            if let Some(want) = from_return {
                if want != ret {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        format!(
                            "lambda returns {}, but its body has type {}",
                            self.show(want),
                            self.show(ret)
                        ),
                    ));
                }
            }
            if escaped_ref(&self.ty_to_parsed(ret)) {
                return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    "a function cannot return a reference",
                ));
            }
            self.sigs.get_mut(name).unwrap().ret = Some(ret);
        }

        // Validate and publish the final result, not the provisional recursive
        // signature. Editor facts must agree with the checked callable type.
        if func.name == "main" && !matches!(ret, Ty::Unit | Ty::I16) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("main returns unit or i16, not {}", self.show(ret)),
            ));
        }
        if func.type_params.is_empty() && func.name_span.end > func.name_span.start {
            let params = self.sigs[name]
                .params
                .iter()
                .map(|ty| self.show(*ty))
                .collect::<Vec<_>>()
                .join(", ");
            let signature = format!("fn({params}) -> {}", self.show(async_result.unwrap_or(ret)));
            if let Some(fact) = self
                .facts
                .iter_mut()
                .find(|fact| fact.def == func.name_span)
            {
                fact.ty = signature;
            }
        }

        if func.body.tail.is_none() && ret != Ty::Unit && !always_returns(&func.body) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("`{}` must return {}", func.name, self.show(ret)),
            ));
        }
        if func.body.tail.is_some() && !returning_tail && body_ty != ret {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!(
                    "`{}` returns {}, the body has type {}",
                    func.name,
                    self.show(ret),
                    self.show(body_ty)
                ),
            ));
        }
        if let Some(err) = self.capture_error.take() {
            return Err(err);
        }
        let shared: Vec<bool> = self.bindings.iter().map(|binding| binding.shared).collect();
        let bindings = self.bindings.iter().map(|binding| binding.ty).collect();
        if func.c_export {
            let params = self.sigs[name].params.clone();
            for ty in params {
                self.require_c_type(func.name_span.start, ty, false)?;
            }
            self.require_c_type(func.name_span.start, ret, true)?;
        }
        let captures = std::mem::take(&mut self.captures);
        self.typed.push(TFn {
            async_context: self.async_context.flatten(),
            async_result,
            name: name.to_string(),
            params,
            ret,
            bindings,
            body,
            closure,
            captures,
            shared,
            c_export: func.c_export,
        });
        self.scopes = saved.scopes;
        self.bindings = saved.bindings;
        self.fn_ret = saved.fn_ret;
        self.lambda_return = saved_lambda_return;
        self.loops = saved.loops;
        self.outer_scopes = saved_outer_scopes;
        self.outer_bindings = saved_outer_bindings;
        self.captures = saved_captures;
        self.shared_spans = saved_shared;
        self.borrows = saved_borrows;
        self.loose_receiver = saved_loose;
        self.assoc_self = saved_assoc_self;
        self.async_context = saved_async_context;
        Ok(())
    }

    pub(super) fn instantiate(&mut self, name: &str, args: &[Ty]) -> Result<String, CompileError> {
        if self.generic_check && !self.signature_only.contains(name) {
            self.check_declaration(name)?;
        }
        let key = (name.to_string(), args.to_vec());
        if let Some(mangled) = self.fn_mono.get(&key) {
            return Ok(mangled.clone());
        }
        let mangled = self.mangle(name, args);
        self.fn_mono.insert(key, mangled.clone());
        let index = self.fn_schemas[name].index;
        let type_params = self.fn_schemas[name].type_params.clone();
        let func = self.ast_fns[index].clone();
        self.check_bounds(&func.type_params, args, func.name_span.start)?;
        let mut subst = HashMap::new();
        for (param, arg) in type_params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        let mut params = Vec::new();
        for param in &func.params {
            params.push(self.apply(param.ty.as_ref().unwrap(), &subst)?);
        }
        let ret = self.apply(func.ret.as_ref().unwrap(), &subst)?;
        self.sigs.insert(
            mangled.clone(),
            Sig {
                params,
                ret: Some(ret),
                c_abi: false,
                variadic: false,
            },
        );
        if self.signature_only.contains(name) {
            return Ok(mangled);
        }
        let saved_subst = std::mem::replace(&mut self.subst, subst);
        self.check_fn(&func, &mangled, true, false)?;
        self.subst = saved_subst;
        Ok(mangled)
    }

    pub(super) fn mangle(&self, name: &str, args: &[Ty]) -> String {
        let mut mangled = name.to_string();
        for arg in args {
            mangled.push('$');
            mangled.push_str(&self.mangle_ty(*arg));
        }
        mangled
    }

    pub(super) fn mangle_ty(&self, ty: Ty) -> String {
        match ty {
            Ty::Constructor(id) => format!("constructor${id}$"),
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
            Ty::Owned(id) => format!("Owned${}$", self.mangle_ty(self.owner_elems[id as usize])),
            Ty::Vec(id) => format!("Vec${}$", self.mangle_ty(self.vec_elems[id as usize])),
            Ty::Slice(id) => format!("Slice${}$", self.mangle_ty(self.slice_elems[id as usize])),
            Ty::Array { id, .. } => {
                let info = &self.arrays[id as usize];
                format!("arr${}${}$", info.len, self.mangle_ty(info.elem))
            }
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                let prefix = if refty.mutable { "refmut$" } else { "ref$" };
                format!("{prefix}{}", self.mangle_ty(refty.inner))
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                let prefix = if ptr.mutable { "ptrmut$" } else { "ptr$" };
                format!("{prefix}{}$", self.mangle_ty(ptr.inner))
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let mut text = String::from(if sig.once { "oncefn" } else { "fn" });
                for param in &sig.params {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*param));
                }
                text.push('$');
                text.push_str(&self.mangle_ty(sig.ret));
                text.push('$');
                text
            }
            Ty::Struct { id, .. } => format!("{}$", self.structs[id as usize].name),
            Ty::Tuple { id, .. } => {
                let mut text = String::from("tup");
                for field in &self.tuples[id as usize] {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*field));
                }
                text.push('$');
                text
            }
            other => other.to_string(),
        }
    }
}
