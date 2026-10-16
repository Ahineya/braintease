//! Facts in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_expr(
        &mut self,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let saved = self.current_span;
        self.current_span = expr.span;
        let typed = self.check_expr_kind(expr, expected)?;
        let ty = self.show(typed.ty);
        self.expr_types.push(ExprType {
            span: expr.span,
            ty,
        });
        if self.editor {
            self.note_trait_completions(expr.span, typed.ty);
        }
        self.current_span = saved;
        Ok(typed)
    }

    pub(super) fn note_type(&mut self, span: Span, name: &str) {
        let Some(def) = self.type_defs.get(name).copied() else {
            return;
        };
        if def.end <= def.start || span.end <= span.start {
            return;
        }
        self.uses.push(NameUse {
            span,
            def,
            def_file: None,
            ty: format!("type {name}"),
        });
    }

    pub(super) fn note_sig(&mut self, label: String) {
        let span = self.current_span;
        if span.end <= span.start {
            return;
        }
        if let Some(found) = self.call_sigs.iter_mut().find(|sig| sig.span == span) {
            found.label = label;
        } else {
            self.call_sigs.push(CallSig { span, label });
        }
    }

    pub(super) fn noted_fn(&mut self, params: &[Ty], ret: Ty, skip_first: bool) {
        let mut shown: Vec<String> = params.iter().map(|ty| self.show(*ty)).collect();
        if skip_first && !shown.is_empty() {
            shown.remove(0);
        }
        self.note_sig(format!("fn({}) -> {}", shown.join(", "), self.show(ret)));
    }

    pub(super) fn show(&self, ty: Ty) -> String {
        if let Some((name, _)) = self.opaque.get(&ty) {
            return name.clone();
        }
        match ty {
            Ty::Adt(id) => self.adts[id as usize].name.clone(),
            Ty::Owned(id) => format!("Owned<{}>", self.show(self.owner_elems[id as usize])),
            Ty::Vec(id) => format!("Vec<{}>", self.show(self.vec_elems[id as usize])),
            Ty::Slice(id) => format!("Slice<{}>", self.show(self.slice_elems[id as usize])),
            Ty::Array { id, len, .. } => {
                format!("[{}; {len}]", self.show(self.arrays[id as usize].elem))
            }
            Ty::Struct { id, .. } => self.structs[id as usize].name.clone(),
            Ty::Tuple { id, .. } => {
                let fields: Vec<String> = self.tuples[id as usize]
                    .iter()
                    .map(|ty| self.show(*ty))
                    .collect();
                format!("({})", fields.join(", "))
            }
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                let prefix = if refty.mutable { "&mut " } else { "&" };
                format!("{prefix}{}", self.show(refty.inner))
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                let prefix = if ptr.mutable { "*mut " } else { "*const " };
                format!("{prefix}{}", self.show(ptr.inner))
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let params: Vec<String> =
                    sig.params.iter().map(|param| self.show(*param)).collect();
                format!("fn({}) -> {}", params.join(", "), self.show(sig.ret))
            }
            Ty::CFn(id) => {
                let sig = &self.cfns[id as usize];
                let params: Vec<String> =
                    sig.params.iter().map(|param| self.show(*param)).collect();
                format!(
                    "extern \"C\" fn({}) -> {}",
                    params.join(", "),
                    self.show(sig.ret)
                )
            }
            other => other.to_string(),
        }
    }

    pub(super) fn vec_elem_of(&self, expr: &Expr) -> Option<Ty> {
        let ty = self.peek_place(expr)?;
        self.element_of(ty).or_else(|| {
            let Ty::Ref(id) = ty else {
                return None;
            };
            self.element_of(self.refs[id as usize].inner)
        })
    }

    pub(super) fn peek_place(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Name(name) => self.binding_ty(name),
            ExprKind::Field { base, name, .. } => {
                let ty = self.peek_place(base)?;
                self.field_ty(ty, name)
            }
            ExprKind::Index { base, .. } => {
                let ty = self.peek_place(base)?;
                self.element_of(self.referent(ty))
            }
            ExprKind::Deref(base) => match self.peek_place(base)? {
                Ty::Ref(id) => Some(self.refs[id as usize].inner),
                Ty::Ptr(id) => Some(self.ptrs[id as usize].inner),
                _ => None,
            },
            _ => None,
        }
    }

    pub(super) fn binding_ty(&self, name: &str) -> Option<Ty> {
        if let Some(id) = self.lookup(name) {
            return Some(self.bindings[id as usize].ty);
        }
        let id = self
            .outer_scopes
            .iter()
            .rev()
            .find_map(|scope| scope.get(name).copied())?;
        Some(self.outer_bindings[id as usize].ty)
    }

    pub(super) fn field_ty(&self, ty: Ty, name: &str) -> Option<Ty> {
        let Ty::Struct { id, .. } = self.referent(ty) else {
            return None;
        };
        self.structs[id as usize]
            .fields
            .iter()
            .find(|(field, _)| field == name)
            .map(|(_, ty)| *ty)
    }

    pub(super) fn peek(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Int(_) | ExprKind::Float(_) => None,
            ExprKind::Bool(_) => Some(Ty::Bool),
            ExprKind::Name(name) => self
                .lookup(name)
                .map(|id| self.bindings[id as usize].ty)
                .or_else(|| {
                    let id = self
                        .outer_scopes
                        .iter()
                        .rev()
                        .find_map(|scope| scope.get(name).copied())?;
                    Some(self.outer_bindings[id as usize].ty)
                })
                .or_else(|| self.consts.get(name).map(|(ty, _)| *ty)),
            ExprKind::Borrow { .. }
            | ExprKind::Try(_)
            | ExprKind::Slice { .. }
            | ExprKind::Deref(_) => None,
            ExprKind::Assoc {
                ty_name, method, ..
            } => {
                if let Some(name) = self.trait_name(ty_name) {
                    self.trait_return_hint(&name, method)
                } else {
                    let found = self
                        .methods
                        .iter()
                        .find(|m| m.type_name == *ty_name && m.method == *method)?;
                    self.sigs.get(&found.func).and_then(|s| s.ret)
                }
            }
            ExprKind::Unary {
                op: UnaryOp::Not, ..
            } => Some(Ty::Bool),
            ExprKind::Unary {
                op: UnaryOp::Neg | UnaryOp::BitNot,
                expr,
            } => self.peek(expr),
            ExprKind::Binary { op, .. } if op.is_compare() || op.is_logic() => Some(Ty::Bool),
            ExprKind::Binary { lhs, rhs, .. } => self.peek(lhs).or_else(|| self.peek(rhs)),
            ExprKind::Str(_) => Some(Ty::String),
            ExprKind::Char(_) => Some(Ty::Char),
            ExprKind::CStr(_) | ExprKind::Lambda { .. } => None,
            ExprKind::Call { callee, .. } if self.binding_ty(callee).is_some() => {
                match self.binding_ty(callee)? {
                    Ty::Func(id) => Some(self.funcs[id as usize].ret),
                    Ty::CFn(id) => Some(self.cfns[id as usize].ret),
                    _ => None,
                }
            }
            ExprKind::Call { callee, .. }
                if matches!(callee.as_str(), "from_char" | "from_byte") =>
            {
                Some(Ty::String)
            }
            ExprKind::Call { callee, args, .. } if callee == "move" => {
                args.first().and_then(|e| self.peek(e))
            }
            ExprKind::Call { callee, .. } if callee == "owned" => None,
            ExprKind::Call { callee, .. } if is_builtin(callee) => Some(Ty::Unit),
            ExprKind::Call { callee, .. } => self.sigs.get(callee).and_then(|sig| sig.ret),
            ExprKind::Method {
                receiver,
                method,
                resolved,
                ..
            } => {
                if method == "len" && self.vec_elem_of(receiver).is_some() {
                    Some(Ty::Usize)
                } else if method == "is_empty" && self.vec_elem_of(receiver).is_some() {
                    Some(Ty::Bool)
                } else if (method == "push" || method == "clear")
                    && self.vec_elem_of(receiver).is_some()
                {
                    Some(Ty::Unit)
                } else {
                    if let Some(ty) = self.peek(receiver) {
                        if method == "into_inner" {
                            if let Ty::Owned(id) = ty {
                                return Some(self.owner_elems[id as usize]);
                            }
                        }
                        if let Some(key) = self.type_key(ty) {
                            if let Some(found) = self
                                .methods
                                .iter()
                                .find(|m| m.type_name == key && m.method == *method)
                            {
                                return self.sigs.get(&found.func).and_then(|s| s.ret);
                            }
                        }
                        if let Ok(candidates) = self.trait_candidates(expr.span.start, ty, method) {
                            if candidates.len() == 1 {
                                return self.trait_return_hint(&candidates[0], method);
                            }
                        }
                    }
                    self.sigs.get(resolved).and_then(|sig| sig.ret)
                }
            }
            ExprKind::Index { base, .. } => self
                .peek_place(base)
                .and_then(|ty| self.element_of(self.referent(ty))),
            ExprKind::Array(_) => None,
            ExprKind::If {
                else_body: None, ..
            }
            | ExprKind::While { .. }
            | ExprKind::For { .. } => Some(Ty::Unit),
            ExprKind::Tuple(_) | ExprKind::StructLit { .. } | ExprKind::Range { .. } => None,
            ExprKind::Field { base, name, .. } => {
                self.peek_place(base).and_then(|ty| self.field_ty(ty, name))
            }
            ExprKind::Match { arms, .. } => arms.first().and_then(|arm| self.peek(&arm.body)),
            ExprKind::If {
                then_body,
                else_body: Some(else_body),
                ..
            } => self.peek_block(then_body).or_else(|| self.peek(else_body)),
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.peek_block(block),
            ExprKind::Cast { ty, .. } => match ty {
                ParsedTy::Named { name, args, .. } if args.is_empty() => Ty::parse_name(name),
                _ => None,
            },
        }
    }

    pub(super) fn peek_block(&self, block: &Block) -> Option<Ty> {
        match &block.tail {
            Some(expr) => self.peek(expr),
            None => Some(Ty::Unit),
        }
    }
}
