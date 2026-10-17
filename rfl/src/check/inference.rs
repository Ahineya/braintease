//! Inference in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn infer_omitted(&mut self, functions: &mut [Function]) -> Result<(), CompileError> {
        for func in functions.iter_mut() {
            for param in &mut func.params {
                if param.ty.is_none() {
                    if let Pat::Struct { name, span, .. } = &param.pat {
                        param.ty = Some(ParsedTy::Named {
                            span: *span,
                            name: name.clone(),
                            args: Vec::new(),
                        });
                    }
                }
            }
        }
        let pending: Vec<usize> = functions
            .iter()
            .enumerate()
            .filter(|(_, func)| {
                func.type_params.is_empty() && func.params.iter().any(|param| param.ty.is_none())
            })
            .map(|(index, _)| index)
            .collect();
        if pending.is_empty() {
            return Ok(());
        }
        let mut solver = Solver::new();
        let mut sigs: HashMap<String, (Vec<u32>, u32)> = HashMap::new();
        for index in &pending {
            let func = &functions[*index];
            let mut params = Vec::new();
            for param in &func.params {
                let var = match &param.ty {
                    Some(ty) => self.parsed_var(&mut solver, ty, &HashMap::new())?,
                    None => solver.fresh(),
                };
                params.push(var);
            }
            let ret = match &func.ret {
                Some(ty) => self.parsed_var(&mut solver, ty, &HashMap::new())?,
                None => solver.fresh(),
            };
            sigs.insert(func.name.clone(), (params, ret));
        }
        for index in &pending {
            let func = functions[*index].clone();
            let (params, ret) = sigs[&func.name].clone();
            let body_ret = if func.is_async {
                match solver.info[solver.find(ret) as usize].as_ref() {
                    Some(IInfo::Adt { args, .. }) => args[0],
                    _ => unreachable!("async result is wrapped by the parser"),
                }
            } else {
                ret
            };
            let mut scope = vec![HashMap::new()];
            for (param, var) in func.params.iter().zip(&params) {
                self.infer_pat(&mut solver, &param.pat, *var, &mut scope)?;
            }
            let body = self.infer_block(
                &mut solver,
                &func.body,
                &sigs,
                functions,
                &mut scope,
                body_ret,
            )?;
            solver.unify(body, body_ret).map_err(|_| {
                CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("cannot infer the type of `{}`", func.name),
                )
            })?;
        }
        solver.default_numbers();
        for index in pending {
            let (params, ret) = sigs[&functions[index].name].clone();
            let mut free = Vec::new();
            let mut seen = HashMap::new();
            for var in params.iter().copied().chain(std::iter::once(ret)) {
                self.collect_free(&solver, var, &mut free, &mut seen);
            }
            if functions[index].name == "main" && !free.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    functions[index].name_span.start,
                    "main cannot be generic",
                ));
            }
            let mut names = HashMap::new();
            for (slot, var) in free.iter().enumerate() {
                names.insert(*var, type_var_name(slot));
            }
            let func = &mut functions[index];
            func.type_params = free
                .iter()
                .enumerate()
                .map(|(slot, _)| TypeParam {
                    bounds: Vec::new(),
                    name: type_var_name(slot),
                    span: func.name_span,
                })
                .collect();
            for (param, var) in func.params.iter_mut().zip(&params) {
                param.ty = Some(self.render_var(&solver, *var, &names)?);
            }
            func.ret = Some(self.render_var(&solver, ret, &names)?);
        }
        Ok(())
    }

    pub(super) fn parsed_var(
        &mut self,
        solver: &mut Solver,
        ty: &ParsedTy,
        subst: &HashMap<String, u32>,
    ) -> Result<u32, CompileError> {
        match ty {
            ParsedTy::Projection { span, .. } => {
                let mut concrete = HashMap::new();
                for (name, var) in subst {
                    let parsed = self.render_var(solver, *var, &HashMap::new()).map_err(|_| CompileError::at(self.source, span.start, "cannot infer an associated type before its base type; add parameter types"))?;
                    concrete.insert(name.clone(), self.apply(&parsed, &HashMap::new())?);
                }
                let ty = self.apply(ty, &concrete)?;
                Ok(solver.concrete(ty))
            }
            ParsedTy::Named { span, name, args } => {
                if let Some(var) = subst.get(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(*var);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(solver.concrete(prim));
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(solver.structure(id));
                }
                if name == "Vec" || name == "Slice" || name == "Owned" {
                    let concrete = self.apply(ty, &HashMap::new())?;
                    return Ok(solver.concrete(concrete));
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                let mut vars = Vec::new();
                for arg in args {
                    vars.push(self.parsed_var(solver, arg, subst)?);
                }
                Ok(solver.adt(schema, vars))
            }
            ParsedTy::Func { params, ret, .. } => {
                let mut vars = Vec::new();
                for param in params {
                    vars.push(self.parsed_var(solver, param, subst)?);
                }
                let ret = self.parsed_var(solver, ret, subst)?;
                Ok(solver.func(vars, ret))
            }
            ParsedTy::Tuple { fields, .. } => {
                let mut vars = Vec::new();
                for field in fields {
                    vars.push(self.parsed_var(solver, field, subst)?);
                }
                Ok(solver.tuple(vars))
            }
            ParsedTy::Ref { span, .. } => Err(CompileError::at(
                self.source,
                span.start,
                "a reference cannot be inferred here",
            )),
            ParsedTy::Ptr { .. } | ParsedTy::CFn { .. } => {
                let concrete = self.apply(ty, &HashMap::new())?;
                Ok(solver.concrete(concrete))
            }
            ParsedTy::Array { span, .. } => {
                let concrete = self.apply(ty, &HashMap::new())?;
                let _ = span;
                Ok(solver.concrete(concrete))
            }
        }
    }

    pub(super) fn infer_block(
        &mut self,
        solver: &mut Solver,
        block: &Block,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<u32, CompileError> {
        for stmt in &block.stmts {
            self.infer_stmt(solver, stmt, sigs, functions, scope, ret)?;
        }
        match &block.tail {
            Some(expr) => self.infer_expr(solver, expr, sigs, functions, scope, ret, None),
            None => Ok(solver.concrete(Ty::Unit)),
        }
    }

    pub(super) fn infer_stmt(
        &mut self,
        solver: &mut Solver,
        stmt: &Stmt,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<(), CompileError> {
        match stmt {
            Stmt::Let {
                pattern, ty, init, ..
            } => {
                let expected = match ty {
                    Some(ty) => Some(self.parsed_var(solver, ty, &HashMap::new())?),
                    None => None,
                };
                let init = self.infer_expr(solver, init, sigs, functions, scope, ret, expected)?;
                if let Some(expected) = expected {
                    solver.unify(init, expected).map_err(|_| {
                        CompileError::at(self.source, pattern.span().start, "incompatible types")
                    })?;
                }
                self.infer_pat(solver, pattern, init, scope)
            }
            Stmt::Return { value, .. } => {
                let got = match value {
                    Some(expr) => {
                        self.infer_expr(solver, expr, sigs, functions, scope, ret, Some(ret))?
                    }
                    None => solver.concrete(Ty::Unit),
                };
                solver
                    .unify(got, ret)
                    .map_err(|_| CompileError::at(self.source, 0, "incompatible types"))?;
                Ok(())
            }
            Stmt::Assign { value, .. } => {
                self.infer_expr(solver, value, sigs, functions, scope, ret, None)?;
                Ok(())
            }
            Stmt::Expr(expr) => {
                self.infer_expr(solver, expr, sigs, functions, scope, ret, None)?;
                Ok(())
            }
            Stmt::Break { .. }
            | Stmt::Continue { .. }
            | Stmt::IndexAssign { .. }
            | Stmt::FieldAssign { .. }
            | Stmt::DerefAssign { .. } => Ok(()),
        }
    }

    pub(super) fn infer_expr(
        &mut self,
        solver: &mut Solver,
        expr: &Expr,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
        expected: Option<u32>,
    ) -> Result<u32, CompileError> {
        let span = expr.span.start;
        let got = match &expr.kind {
            ExprKind::Int(_) => {
                let var = solver.fresh();
                solver.mark_numeric(var);
                var
            }
            ExprKind::Float(_) => {
                let var = solver.fresh();
                solver.mark_float(var);
                var
            }
            ExprKind::CStr(_) => solver.concrete(self.intern_ptr(false, Ty::I16)),
            ExprKind::Str(_) => solver.concrete(Ty::String),
            ExprKind::Char(_) => solver.concrete(Ty::Char),
            ExprKind::Bool(_) => solver.concrete(Ty::Bool),
            ExprKind::Name(name) => self.infer_name(solver, span, name, sigs, functions, scope)?,
            ExprKind::Unary { op, expr: inner } => {
                let inner = self.infer_expr(solver, inner, sigs, functions, scope, ret, None)?;
                match op {
                    UnaryOp::Not => {
                        let boolean = solver.concrete(Ty::Bool);
                        solver.unify(inner, boolean).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                        solver.concrete(Ty::Bool)
                    }
                    UnaryOp::BitNot => {
                        solver.mark_numeric(inner);
                        inner
                    }
                    UnaryOp::Neg => {
                        let root = solver.find(inner);
                        let float_info = matches!(solver.info[root as usize], Some(IInfo::Concrete(ty)) if ty.is_float());
                        if !solver.floaty[root as usize] && !float_info {
                            solver.mark_numeric(inner);
                        }
                        inner
                    }
                }
            }
            ExprKind::Binary { op, lhs, rhs } => {
                let left = self.infer_expr(solver, lhs, sigs, functions, scope, ret, None)?;
                let right = self.infer_expr(solver, rhs, sigs, functions, scope, ret, None)?;
                if matches!(op, BinOp::And | BinOp::Or) {
                    let boolean = solver.concrete(Ty::Bool);
                    solver
                        .unify(left, boolean)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    solver
                        .unify(right, boolean)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    boolean
                } else {
                    solver
                        .unify(left, right)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    if op.is_arith() || op.is_bitwise() {
                        let root = solver.find(left);
                        let float_info = matches!(solver.info[root as usize], Some(IInfo::Concrete(ty)) if ty.is_float());
                        if !solver.floaty[root as usize] && !float_info {
                            solver.mark_numeric(left);
                        }
                        left
                    } else {
                        solver.concrete(Ty::Bool)
                    }
                }
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => {
                scope.push(HashMap::new());
                let ty = self.infer_block(solver, block, sigs, functions, scope, ret)?;
                scope.pop();
                ty
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                let cond = self.infer_expr(solver, cond, sigs, functions, scope, ret, None)?;
                let boolean = solver.concrete(Ty::Bool);
                solver
                    .unify(cond, boolean)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                scope.push(HashMap::new());
                let then_ty = self.infer_block(solver, then_body, sigs, functions, scope, ret)?;
                scope.pop();
                if let Some(else_body) = else_body {
                    let else_ty = self.infer_expr(
                        solver,
                        else_body,
                        sigs,
                        functions,
                        scope,
                        ret,
                        Some(then_ty),
                    )?;
                    solver
                        .unify(then_ty, else_ty)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    then_ty
                } else {
                    solver.concrete(Ty::Unit)
                }
            }
            ExprKind::Match { scrutinee, arms } => {
                let scrut =
                    self.infer_expr(solver, scrutinee, sigs, functions, scope, ret, None)?;
                let mut result = expected;
                for arm in arms {
                    scope.push(HashMap::new());
                    self.infer_pat(solver, &arm.pat, scrut, scope)?;
                    if let Some(guard) = &arm.guard {
                        let guard =
                            self.infer_expr(solver, guard, sigs, functions, scope, ret, None)?;
                        let boolean = solver.concrete(Ty::Bool);
                        solver.unify(guard, boolean).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                    }
                    let arm_ty =
                        self.infer_expr(solver, &arm.body, sigs, functions, scope, ret, result)?;
                    scope.pop();
                    if let Some(want) = result {
                        solver.unify(arm_ty, want).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                    }
                    result = Some(arm_ty);
                }
                result.unwrap_or_else(|| solver.concrete(Ty::Unit))
            }
            ExprKind::Call { callee, args, .. } => {
                self.infer_call(solver, span, callee, args, sigs, functions, scope, ret)?
            }
            ExprKind::Tuple(fields) => {
                let mut vars = Vec::new();
                for field in fields {
                    vars.push(self.infer_expr(solver, field, sigs, functions, scope, ret, None)?);
                }
                solver.tuple(vars)
            }
            ExprKind::StructLit { name, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let info = self.structs[id as usize].fields.clone();
                for (field_name, field_ty) in &info {
                    let Some((_, expr)) = fields.iter().find(|(found, _)| found == field_name)
                    else {
                        continue;
                    };
                    let got = self.infer_expr(solver, expr, sigs, functions, scope, ret, None)?;
                    let want =
                        self.parsed_var(solver, &self.ty_to_parsed(*field_ty), &HashMap::new())?;
                    solver
                        .unify(got, want)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                }
                solver.structure(id)
            }
            other => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("cannot infer parameter types through {other:?}; add parameter types"),
                ));
            }
        };
        if let Some(expected) = expected {
            solver
                .unify(got, expected)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        Ok(got)
    }

    pub(super) fn infer_name(
        &mut self,
        solver: &mut Solver,
        span: usize,
        name: &str,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &[HashMap<String, u32>],
    ) -> Result<u32, CompileError> {
        for map in scope.iter().rev() {
            if let Some(var) = map.get(name) {
                return Ok(*var);
            }
        }
        if let Some((ty, _)) = self.consts.get(name) {
            return Ok(solver.concrete(*ty));
        }
        if self.ctor_schema.contains_key(name) {
            return self.infer_ctor(solver, span, name, &[]);
        }
        if let Some((params, ret)) = sigs.get(name) {
            return Ok(solver.func(params.clone(), *ret));
        }
        if let Some(func) = functions.iter().find(|func| func.name == name) {
            if !func.type_params.is_empty() {
                return Err(CompileError::at(self.source, span, format!("generic function `{name}` needs a concrete signature to be used as a value")));
            }
            let mut params = Vec::new();
            for param in &func.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot infer a type for `{name}`"),
                    ));
                };
                params.push(self.parsed_var(solver, ty, &HashMap::new())?);
            }
            let ret = match &func.ret {
                Some(ty) => self.parsed_var(solver, ty, &HashMap::new())?,
                None => solver.fresh(),
            };
            return Ok(solver.func(params, ret));
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("unknown name `{name}`"),
        ))
    }

    pub(super) fn infer_call(
        &mut self,
        solver: &mut Solver,
        span: usize,
        callee: &str,
        args: &[Expr],
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<u32, CompileError> {
        if callee == "owned" {
            return Err(CompileError::at(
                self.source,
                span,
                "add parameter types to a function that constructs an Owned resource",
            ));
        }
        let mut arg_vars = Vec::new();
        for arg in args {
            arg_vars.push(self.infer_expr(solver, arg, sigs, functions, scope, ret, None)?);
        }
        if callee == "move" {
            if arg_vars.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "move() expects one argument",
                ));
            }
            return Ok(arg_vars[0]);
        }
        if is_builtin(callee) {
            if let Some(arg) = arg_vars.first() {
                if callee == "putc" {
                    solver.mark_numeric(*arg);
                }
            }
            return Ok(solver.concrete(Ty::Unit));
        }
        for map in scope.iter().rev() {
            if let Some(var) = map.get(callee) {
                let result = solver.fresh();
                let func = solver.func(arg_vars, result);
                solver
                    .unify(*var, func)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                return Ok(result);
            }
        }
        if self.ctor_schema.contains_key(callee) {
            return self.infer_ctor(solver, span, callee, &arg_vars);
        }
        if let Some((params, result)) = sigs.get(callee) {
            if params.len() != arg_vars.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {} arguments", params.len()),
                ));
            }
            for (param, arg) in params.iter().zip(arg_vars) {
                solver
                    .unify(*param, arg)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
            }
            return Ok(*result);
        }
        let Some(func) = functions.iter().find(|func| func.name == callee).cloned() else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown function `{callee}`"),
            ));
        };
        if !func.type_params.is_empty() {
            let mut subst = HashMap::new();
            for param in &func.type_params {
                subst.insert(param.name.clone(), solver.fresh());
            }
            if func.params.len() != arg_vars.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {} arguments", func.params.len()),
                ));
            }
            for (param, arg) in func.params.iter().zip(arg_vars) {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                let var = self.parsed_var(solver, ty, &subst)?;
                solver
                    .unify(var, arg)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
            }
            let Some(ty) = &func.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{callee}` needs a return type"),
                ));
            };
            return self.parsed_var(solver, ty, &subst);
        }
        if func.params.len() != arg_vars.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {} arguments", func.params.len()),
            ));
        }
        for (param, arg) in func.params.iter().zip(arg_vars) {
            let Some(ty) = &param.ty else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("cannot infer a type for `{callee}`"),
                ));
            };
            let var = self.parsed_var(solver, ty, &HashMap::new())?;
            solver
                .unify(var, arg)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        match &func.ret {
            Some(ty) => self.parsed_var(solver, ty, &HashMap::new()),
            None => Ok(solver.fresh()),
        }
    }

    pub(super) fn infer_ctor(
        &mut self,
        solver: &mut Solver,
        span: usize,
        name: &str,
        args: &[u32],
    ) -> Result<u32, CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown constructor `{name}`"),
            ));
        };
        let params = self.schemas[schema].params.clone();
        let fields = self.schemas[schema].ctors[tag as usize].clone();
        if fields.len() != args.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments, found {}",
                    fields.len(),
                    args.len()
                ),
            ));
        }
        let mut subst = HashMap::new();
        for param in &params {
            subst.insert(param.clone(), solver.fresh());
        }
        for (field, arg) in fields.iter().zip(args) {
            let var = self.parsed_var(solver, field, &subst)?;
            solver
                .unify(var, *arg)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        let arg_vars: Vec<u32> = params.iter().map(|param| subst[param]).collect();
        Ok(solver.adt(schema, arg_vars))
    }

    pub(super) fn infer_pat(
        &mut self,
        solver: &mut Solver,
        pat: &Pat,
        var: u32,
        scope: &mut Vec<HashMap<String, u32>>,
    ) -> Result<(), CompileError> {
        match pat {
            Pat::Name { name, .. } if name == "_" => Ok(()),
            Pat::Name { name, span } => {
                if self.ctor_schema.contains_key(name) {
                    let ctor = self.infer_ctor(solver, span.start, name, &[])?;
                    return solver.unify(var, ctor).map_err(|_| {
                        CompileError::at(self.source, span.start, "incompatible types")
                    });
                }
                scope.last_mut().unwrap().insert(name.clone(), var);
                Ok(())
            }
            Pat::Ctor { name, span, fields } => {
                let mut field_vars = Vec::new();
                for _ in fields {
                    field_vars.push(solver.fresh());
                }
                let ctor = self.infer_ctor(solver, span.start, name, &field_vars)?;
                solver
                    .unify(var, ctor)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))?;
                for (field, field_var) in fields.iter().zip(field_vars) {
                    self.infer_pat(solver, field, field_var, scope)?;
                }
                Ok(())
            }
            Pat::Int { .. } => {
                solver.mark_numeric(var);
                Ok(())
            }
            Pat::Char { span, .. } => {
                let ch = solver.concrete(Ty::Char);
                solver
                    .unify(var, ch)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))
            }
            Pat::Bool { span, .. } => {
                let boolean = solver.concrete(Ty::Bool);
                solver
                    .unify(var, boolean)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))
            }
            Pat::Tuple { fields, .. } => {
                let mut vars = Vec::new();
                for field in fields {
                    let field_var = solver.fresh();
                    self.infer_pat(solver, field, field_var, scope)?;
                    vars.push(field_var);
                }
                let tuple = solver.tuple(vars);
                solver.unify(var, tuple).map_err(|_| {
                    CompileError::at(self.source, pat.span().start, "incompatible types")
                })
            }
            Pat::Struct { name, span, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let structured = solver.structure(id);
                solver
                    .unify(var, structured)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))?;
                let info = self.structs[id as usize].fields.clone();
                for (field_name, _, field_pat) in fields {
                    if let Some((_, ty)) = info.iter().find(|(found, _)| found == field_name) {
                        let field_var =
                            self.parsed_var(solver, &self.ty_to_parsed(*ty), &HashMap::new())?;
                        self.infer_pat(solver, field_pat, field_var, scope)?;
                    }
                }
                Ok(())
            }
        }
    }

    pub(super) fn collect_free(
        &self,
        solver: &Solver,
        var: u32,
        free: &mut Vec<u32>,
        seen: &mut HashMap<u32, ()>,
    ) {
        let var = solver.find(var);
        match &solver.info[var as usize] {
            None => {
                if seen.insert(var, ()).is_none() {
                    free.push(var);
                }
            }
            Some(IInfo::Concrete(_)) | Some(IInfo::Struct(_)) => {}
            Some(IInfo::Func { params, ret }) => {
                for param in params.clone() {
                    self.collect_free(solver, param, free, seen);
                }
                self.collect_free(solver, *ret, free, seen);
            }
            Some(IInfo::Adt { args, .. }) => {
                for arg in args.clone() {
                    self.collect_free(solver, arg, free, seen);
                }
            }
            Some(IInfo::Tuple(fields)) => {
                for field in fields.clone() {
                    self.collect_free(solver, field, free, seen);
                }
            }
        }
    }

    pub(super) fn render_var(
        &self,
        solver: &Solver,
        var: u32,
        names: &HashMap<u32, String>,
    ) -> Result<ParsedTy, CompileError> {
        let var = solver.find(var);
        if let Some(name) = names.get(&var) {
            return Ok(ParsedTy::Named {
                span: Span::empty(),
                name: name.clone(),
                args: Vec::new(),
            });
        }
        let Some(info) = solver.info[var as usize].clone() else {
            return Err(CompileError::at(self.source, 0, "cannot infer a type"));
        };
        match info {
            IInfo::Concrete(ty) => Ok(self.ty_to_parsed(ty)),
            IInfo::Struct(id) => Ok(ParsedTy::Named {
                span: Span::empty(),
                name: self.structs[id as usize].name.clone(),
                args: Vec::new(),
            }),
            IInfo::Func { params, ret } => {
                let mut parsed = Vec::new();
                for param in params {
                    parsed.push(self.render_var(solver, param, names)?);
                }
                Ok(ParsedTy::Func {
                    span: Span::empty(),
                    params: parsed,
                    ret: Box::new(self.render_var(solver, ret, names)?),
                })
            }
            IInfo::Adt { schema, args } => {
                let mut parsed = Vec::new();
                for arg in args {
                    parsed.push(self.render_var(solver, arg, names)?);
                }
                Ok(ParsedTy::Named {
                    span: Span::empty(),
                    name: self.schemas[schema].name.clone(),
                    args: parsed,
                })
            }
            IInfo::Tuple(fields) => {
                let mut parsed = Vec::new();
                for field in fields {
                    parsed.push(self.render_var(solver, field, names)?);
                }
                Ok(ParsedTy::Tuple {
                    span: Span::empty(),
                    fields: parsed,
                })
            }
        }
    }

    pub(super) fn ty_to_parsed(&self, ty: Ty) -> ParsedTy {
        let span = Span::empty();
        match ty {
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                ParsedTy::Named {
                    span,
                    name: self.schemas[adt.schema].name.clone(),
                    args: adt.args.iter().map(|arg| self.ty_to_parsed(*arg)).collect(),
                }
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                ParsedTy::Func {
                    span,
                    params: sig
                        .params
                        .iter()
                        .map(|param| self.ty_to_parsed(*param))
                        .collect(),
                    ret: Box::new(self.ty_to_parsed(sig.ret)),
                }
            }
            Ty::CFn(id) => {
                let sig = &self.cfns[id as usize];
                ParsedTy::CFn {
                    span,
                    params: sig
                        .params
                        .iter()
                        .map(|param| self.ty_to_parsed(*param))
                        .collect(),
                    ret: Box::new(self.ty_to_parsed(sig.ret)),
                }
            }
            Ty::Owned(id) => ParsedTy::Named {
                span,
                name: "Owned".into(),
                args: vec![self.ty_to_parsed(self.owner_elems[id as usize])],
            },
            Ty::Vec(id) => ParsedTy::Named {
                span,
                name: "Vec".to_string(),
                args: vec![self.ty_to_parsed(self.vec_elems[id as usize])],
            },
            Ty::Slice(id) => ParsedTy::Named {
                span,
                name: "Slice".to_string(),
                args: vec![self.ty_to_parsed(self.slice_elems[id as usize])],
            },
            Ty::Struct { id, .. } => ParsedTy::Named {
                span,
                name: self.structs[id as usize].name.clone(),
                args: Vec::new(),
            },
            Ty::Tuple { id, .. } => ParsedTy::Tuple {
                span,
                fields: self.tuples[id as usize]
                    .iter()
                    .map(|field| self.ty_to_parsed(*field))
                    .collect(),
            },
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                ParsedTy::Ref {
                    span,
                    mutable: refty.mutable,
                    inner: Box::new(self.ty_to_parsed(refty.inner)),
                }
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                ParsedTy::Ptr {
                    span,
                    mutable: ptr.mutable,
                    inner: Box::new(self.ty_to_parsed(ptr.inner)),
                }
            }
            Ty::Array { id, len, .. } => ParsedTy::Array {
                span,
                elem: Box::new(self.ty_to_parsed(self.arrays[id as usize].elem)),
                len: Box::new(Expr {
                    span,
                    kind: ExprKind::Int(len as i64),
                }),
            },
            other => ParsedTy::Named {
                span,
                name: other.to_string(),
                args: Vec::new(),
            },
        }
    }

    pub(super) fn ctor_fields(
        &self,
        name: &str,
        ty: Ty,
        span: usize,
    ) -> Result<(u16, Vec<Ty>), CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown constructor `{name}`"),
            ));
        };
        let Ty::Adt(id) = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not a constructor of {}", self.show(ty)),
            ));
        };
        if self.adts[id as usize].schema != schema {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not a constructor of {}", self.show(ty)),
            ));
        }
        Ok((
            tag,
            self.adts[id as usize].ctors[tag as usize].fields.clone(),
        ))
    }
}
