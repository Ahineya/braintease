//! Statements in the RFL type checker.

use super::*;

impl<'a> Checker<'a> {
    pub(super) fn check_block(
        &mut self,
        block: &Block,
        expected: Option<Ty>,
    ) -> Result<TBlock, CompileError> {
        self.scopes.push(HashMap::new());
        self.scope_spans.push(block.span);
        let mut stmts = Vec::new();
        for stmt in &block.stmts {
            stmts.push(self.check_stmt(stmt)?);
        }
        let tail = match &block.tail {
            Some(expr) => Some(self.check_expr(expr, expected)?),
            None => None,
        };
        if let Some(tail_expr) = &tail {
            if tail_expr.ty.is_ref() {
                let span = block.tail.as_ref().map(|expr| expr.span.start).unwrap_or(0);
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a reference cannot escape this block",
                ));
            }
        }
        self.pop_scope();
        self.scope_spans.pop();
        Ok(TBlock { stmts, tail })
    }

    pub(super) fn check_stmt(&mut self, stmt: &Stmt) -> Result<TStmt, CompileError> {
        match stmt {
            Stmt::Let {
                span: let_span,
                pattern,
                mutable,
                ty,
                init,
            } => {
                self.next_visible = Some(let_span.end);
                let annotated = match ty {
                    Some(parsed) => Some(self.apply(parsed, &HashMap::new())?),
                    None => None,
                };
                let init = self.check_expr(init, annotated)?;
                let span = arm_span(pattern);
                if let Some(want) = annotated {
                    if init.ty != want {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "the pattern has type {want}, the initializer has type {}",
                                init.ty
                            ),
                        ));
                    }
                }
                let pat = self.check_pat(pattern, init.ty, *mutable)?;
                if !self.irrefutable_pat(&pat, init.ty) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "a `let` pattern must be irrefutable",
                    ));
                }
                match pat {
                    TPat::Bind(binding) => Ok(TStmt::Let { binding, init }),
                    TPat::Wild => Ok(TStmt::Expr(init)),
                    other => Ok(TStmt::Destructure { pat: other, init }),
                }
            }
            Stmt::Assign {
                name,
                name_span,
                value,
            } => {
                let Some(id) = self.resolve(name) else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown name `{name}`"),
                    ));
                };
                let ty = self.bindings[id as usize].ty;
                if ty.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        "a reference cannot be reassigned",
                    ));
                }
                if !self.bindings[id as usize].mutable {
                    let captured = self.captures.iter().any(|cap| cap.local == id);
                    let message = if captured {
                        format!("`{name}` was captured by value and cannot be assigned")
                    } else {
                        format!("`{name}` is not mutable")
                    };
                    return Err(CompileError::at(self.source, name_span.start, message));
                }
                self.note_use(name_span.start, id, true)?;
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
            Stmt::DerefAssign { ptr, value } => self.check_ptr_store(ptr, None, value),
            Stmt::IndexAssign {
                name,
                name_span,
                fields,
                index,
                value,
            } => {
                if !fields.is_empty() {
                    return self.check_field_index(name, name_span.start, fields, index, value);
                }
                let Some(id) = self.resolve(name) else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown name `{name}`"),
                    ));
                };
                let ty = self.bindings[id as usize].ty;
                if let Ty::Ptr(pid) = ty {
                    self.require_unsafe(name_span.start)?;
                    let info = &self.ptrs[pid as usize];
                    if !info.mutable {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            "cannot store through `*const`",
                        ));
                    }
                    let elem = info.inner;
                    self.require_pointee(name_span.start, elem)?;
                    let stride = elem.cells();
                    let index = self.check_expr(index, Some(Ty::Usize))?;
                    if index.ty != Ty::Usize {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("expected usize, found {}", self.show(index.ty)),
                        ));
                    }
                    let value = self.check_expr(value, Some(elem))?;
                    if value.ty != elem {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!(
                                "expected {}, found {}",
                                self.show(elem),
                                self.show(value.ty)
                            ),
                        ));
                    }
                    let ptr = TExpr {
                        ty,
                        kind: TExprKind::Name(id),
                    };
                    return Ok(TStmt::PtrStore {
                        ptr,
                        index: Some(index),
                        value,
                        stride,
                    });
                }
                if matches!(ty, Ty::Slice(_)) || matches!(self.referent(ty), Ty::Slice(_)) {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        "slices are read-only",
                    ));
                }
                if !ty.is_ref() {
                    self.note_use(name_span.start, id, true)?;
                }
                if !self.place_mutable(id, ty) {
                    let message = if ty.is_ref() {
                        format!("`{name}` needs a mutable reference")
                    } else {
                        format!("`{name}` is not mutable")
                    };
                    return Err(CompileError::at(self.source, name_span.start, message));
                }
                let elem = if let Ty::Vec(vec_id) = self.referent(ty) {
                    self.vec_elems[vec_id as usize]
                } else if let Ty::Array { id: array_id, .. } = ty {
                    self.arrays[array_id as usize].elem
                } else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("`{name}` is not a vector or array"),
                    ));
                };
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                let value = self.check_expr(value, Some(elem))?;
                if value.ty != elem {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!(
                            "expected {}, found {}",
                            self.show(elem),
                            self.show(value.ty)
                        ),
                    ));
                }
                Ok(TStmt::IndexAssign {
                    binding: id,
                    offset: 0,
                    index,
                    value,
                    elem,
                    container: ty,
                })
            }
            Stmt::FieldAssign {
                name,
                name_span,
                fields,
                value,
            } => self.check_field_assign(name, name_span.start, fields, value),
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
                        format!(
                            "return has type {got}, the function returns {}",
                            self.fn_ret
                        ),
                    ));
                }
                Ok(TStmt::Return(value))
            }
            Stmt::Break { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "`break` is outside a loop",
                    ));
                }
                Ok(TStmt::Break)
            }
            Stmt::Continue { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "`continue` is outside a loop",
                    ));
                }
                Ok(TStmt::Continue)
            }
        }
    }
}
