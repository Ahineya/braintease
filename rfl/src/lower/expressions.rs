//! Checked expressions to activation-record instructions.

use super::*;

impl FnBuild {
    pub(super) fn emit_expr_to(&mut self, expr: &TExpr, dest: usize) {
        if self.dead {
            return;
        }
        match &expr.kind {
            TExprKind::Int(value) => self.insn(Insn::Const {
                slot: dest,
                value: expr.ty.bits(*value),
            }),
            TExprKind::Float(bits) => {
                let value = if expr.ty == Ty::F32 {
                    (*bits as u32) as i64
                } else {
                    *bits as i64
                };
                self.insn(Insn::Const { slot: dest, value });
            }
            TExprKind::Bool(value) => self.insn(Insn::Const {
                slot: dest,
                value: i64::from(*value),
            }),
            TExprKind::Char(scalar) => self.insn(Insn::Const {
                slot: dest,
                value: i64::from(*scalar),
            }),
            TExprKind::Str(bytes) => self.insn(Insn::Str {
                dest,
                bytes: bytes.clone(),
            }),
            TExprKind::StrConcat { lhs, rhs } => {
                let lhs = self.emit_expr(lhs);
                if self.dead {
                    return;
                }
                let rhs = self.emit_expr(rhs);
                if self.dead {
                    return;
                }
                self.insn(Insn::StrConcat { dest, lhs, rhs });
            }
            TExprKind::StrSlice {
                base,
                start,
                end,
                inclusive,
            } => {
                let base = self.emit_expr(base);
                if self.dead {
                    return;
                }
                let start = self.emit_expr(start);
                if self.dead {
                    return;
                }
                let end = self.emit_expr(end);
                if self.dead {
                    return;
                }
                self.insn(Insn::StrSlice {
                    dest,
                    base,
                    start,
                    end,
                    inclusive: *inclusive,
                });
            }
            TExprKind::StrFromChar(src) => {
                let src = self.emit_expr(src);
                if self.dead {
                    return;
                }
                self.insn(Insn::StrFromChar { dest, src });
            }
            TExprKind::StrFromByte(src) => {
                let src = self.emit_expr(src);
                if self.dead {
                    return;
                }
                self.insn(Insn::StrFromByte { dest, src });
            }
            TExprKind::Name(id, ..) => {
                let src = self.binding_slot[*id as usize];
                if src != dest {
                    self.insn(Insn::Copy { dest, src });
                }
            }
            TExprKind::Move { binding, .. } => {
                let src = self.binding_slot[*binding as usize];
                self.insn(Insn::Move { dest, src });
            }
            TExprKind::Owned { value, cleanup } => {
                let value_slot = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let cleanup = self.emit_expr(cleanup);
                if self.dead {
                    return;
                }
                self.insn(Insn::Owned {
                    dest,
                    value: value_slot,
                    cleanup,
                    inner: value.ty,
                });
            }
            TExprKind::Replace { place, value } => {
                let place = self.emit_expr(place);
                if self.dead {
                    return;
                }
                let value = self.emit_expr(value);
                if self.dead {
                    return;
                }
                // Keep the old storage distinct even when the enclosing
                // assignment's destination aliases the replaced place.
                let result = self.alloc(expr.ty);
                self.insn(Insn::Replace {
                    dest: result,
                    place,
                    value,
                });
                self.insn(Insn::Move { dest, src: result });
            }
            TExprKind::RawBorrow(value) => {
                self.emit_expr_to(value, dest);
            }
            TExprKind::EraseOwner(value) => {
                let src = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let Ty::Owned(owner) = value.ty else {
                    unreachable!()
                };
                self.insn(Insn::EraseOwner { dest, src, owner });
            }
            TExprKind::IntoInner(value) => {
                let src = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let result = self.alloc(expr.ty);
                self.insn(Insn::IntoInner { dest: result, src });
                self.insn(Insn::Move { dest, src: result });
            }
            TExprKind::Drop(value) => {
                let slot = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::Drop { slot });
            }
            TExprKind::Unary {
                op: UnaryOp::Neg,
                expr: inner,
            } => {
                if let TExprKind::Int(value) = inner.kind {
                    self.insn(Insn::Const {
                        slot: dest,
                        value: expr.ty.bits(value),
                    });
                } else {
                    let src = self.emit_expr(inner);
                    self.insn(Insn::Neg { dest, src });
                }
            }
            TExprKind::Unary {
                op: UnaryOp::Not,
                expr: inner,
            } => {
                let src = self.emit_expr(inner);
                self.insn(Insn::Not { dest, src });
            }
            TExprKind::Unary {
                op: UnaryOp::BitNot,
                expr: inner,
            } => {
                let src = self.emit_expr(inner);
                self.insn(Insn::BitNot { dest, src });
            }
            TExprKind::CFn { symbol } => self.insn(Insn::CFn {
                dest,
                symbol: symbol.clone(),
            }),
            TExprKind::CApply { callee, args } => {
                let slot = self.emit_expr(callee);
                if self.dead {
                    return;
                }
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::CApply {
                    dest,
                    callee: slot,
                    args: arg_slots,
                    ret: expr.ty,
                });
            }
            TExprKind::Binary { op, lhs, rhs } if matches!(op, BinOp::And | BinOp::Or) => {
                self.emit_logic(*op, lhs, rhs, dest);
            }
            TExprKind::Binary { op, lhs, rhs } => {
                if let (TExprKind::Int(a), TExprKind::Int(b)) = (&lhs.kind, &rhs.kind) {
                    match fold_bin(*op, expr.ty, lhs.ty, *a, *b) {
                        Some(value) => self.insn(Insn::Const {
                            slot: dest,
                            value: expr.ty.bits(value),
                        }),
                        None => self.terminate(Term::Fail),
                    }
                    return;
                }
                let left = self.emit_expr(lhs);
                let right = self.emit_expr(rhs);
                self.insn(Insn::Bin {
                    dest,
                    op: *op,
                    lhs: left,
                    rhs: right,
                });
            }
            TExprKind::Apply {
                callee,
                args,
                params,
            } => {
                let slot = self.emit_expr(callee);
                if self.dead {
                    return;
                }
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
                }
                let next = self.fresh();
                self.segs[self.cur].term = Some(Term::Apply {
                    slot,
                    args: arg_slots,
                    arg_tys: params.clone(),
                    dest,
                    next,
                });
                self.cur = next;
                self.dead = false;
            }
            TExprKind::Closure { func } => self.insn(Insn::Closure {
                dest,
                func: func.clone(),
            }),
            TExprKind::ArrayLit { elems, elem } => {
                let mut slots = Vec::new();
                for elem in elems {
                    slots.push(self.emit_expr(elem));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::ArrayLit {
                    dest,
                    elems: slots,
                    elem: *elem,
                });
            }
            TExprKind::Slice {
                base,
                start,
                end,
                inclusive,
                elem,
            } => {
                let base = self.emit_expr(base);
                if self.dead {
                    return;
                }
                let start = self.emit_expr(start);
                if self.dead {
                    return;
                }
                let end = self.emit_expr(end);
                if self.dead {
                    return;
                }
                self.insn(Insn::SliceNew {
                    dest,
                    base,
                    start,
                    end,
                    inclusive: *inclusive,
                    elem: *elem,
                });
            }
            TExprKind::ToVec { src, elem } => {
                let src = self.emit_expr(src);
                if self.dead {
                    return;
                }
                self.insn(Insn::ToVec {
                    dest,
                    src,
                    elem: *elem,
                });
            }
            TExprKind::VecLit { elems, elem } => {
                let mut slots = Vec::new();
                for elem in elems {
                    slots.push(self.emit_expr(elem));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::VecLit {
                    dest,
                    elems: slots,
                    elem: *elem,
                });
            }
            TExprKind::Index { base, index, elem } => {
                let vec = self.read_collection(base, stable_index(index));
                if self.dead {
                    return;
                }
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                self.insn(Insn::Index {
                    dest,
                    vec,
                    index,
                    elem: *elem,
                });
            }
            TExprKind::Len { vec } => {
                let vec = self.read_collection(vec, true);
                if self.dead {
                    return;
                }
                self.insn(Insn::Len { dest, vec });
            }
            TExprKind::Empty { vec } => {
                let vec = self.read_collection(vec, true);
                if self.dead {
                    return;
                }
                self.insn(Insn::Empty { dest, vec });
            }
            TExprKind::Clear {
                binding,
                offset,
                elem,
            } => {
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Clear {
                    elem: *elem,
                    vec,
                    offset: *offset,
                });
            }
            TExprKind::Pop {
                binding,
                offset,
                elem,
                some_tag,
                none_tag,
            } => {
                let value = self.alloc(*elem);
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Pop {
                    vec,
                    offset: *offset,
                    value,
                    dest,
                    elem: *elem,
                    some_tag: *some_tag,
                    none_tag: *none_tag,
                });
            }
            TExprKind::Deref { ptr, index, stride } => {
                let ptr = self.emit_expr(ptr);
                if self.dead {
                    return;
                }
                let index = match index {
                    Some(index) => {
                        let index = self.emit_expr(index);
                        if self.dead {
                            return;
                        }
                        Some(index)
                    }
                    None => None,
                };
                self.insn(Insn::PtrLoad {
                    dest,
                    ptr,
                    index,
                    stride: *stride,
                    cells: expr.ty.cells(),
                });
            }
            TExprKind::PtrOffset { ptr, index, stride } => {
                let ptr = self.emit_expr(ptr);
                if self.dead {
                    return;
                }
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                self.insn(Insn::PtrOff {
                    dest,
                    ptr,
                    index,
                    stride: *stride,
                });
            }
            TExprKind::PtrAddr { ptr } => {
                let ptr = self.emit_expr(ptr);
                if self.dead {
                    return;
                }
                self.insn(Insn::PtrWord { dest, ptr, cell: 0 });
            }
            TExprKind::PtrBank { ptr } => {
                let ptr = self.emit_expr(ptr);
                if self.dead {
                    return;
                }
                self.insn(Insn::PtrWord { dest, ptr, cell: 1 });
            }
            TExprKind::PtrNull { ptr } => {
                let ptr = self.emit_expr(ptr);
                if self.dead {
                    return;
                }
                self.insn(Insn::PtrNull { dest, ptr });
            }
            TExprKind::Borrow {
                binding,
                mutable,
                projections,
                ..
            } => {
                let mut slot = self.binding_slot[*binding as usize];
                let mut indirect = self.slots[slot].ty.is_ref();
                let mut steps = Vec::new();
                for step in projections {
                    match step {
                        TPlaceStep::Owned => steps.push(PlaceStep::Owned),
                        TPlaceStep::Field { offset } => {
                            steps.push(PlaceStep::Field { offset: *offset })
                        }
                        TPlaceStep::Index {
                            index,
                            container,
                            elem,
                        } => {
                            let at = self.emit_expr(index);
                            if self.dead {
                                return;
                            }
                            steps.push(PlaceStep::Index {
                                index: at,
                                container: *container,
                                elem: *elem,
                            });
                            // Check and detach this ancestor before evaluating
                            // the next index. Intermediate addresses live in the
                            // frame even when that index makes a function call.
                            let next = self.alloc(expr.ty);
                            self.insn(Insn::Borrow {
                                dest: next,
                                mutable: *mutable,
                                place: Place {
                                    root: slot,
                                    indirect,
                                    projections: std::mem::take(&mut steps),
                                },
                            });
                            slot = next;
                            indirect = true;
                        }
                    }
                }
                self.insn(Insn::Borrow {
                    dest,
                    mutable: *mutable,
                    place: Place {
                        root: slot,
                        indirect,
                        projections: steps,
                    },
                });
            }
            TExprKind::Try {
                expr,
                ok_tag,
                fail_tag,
                payload,
                fail,
            } => {
                let src = self.emit_expr(expr);
                if self.dead {
                    return;
                }
                let result = self
                    .result
                    .expect("? is used in a function that returns a value");
                let fail_slot = fail.map(|ty| self.alloc(ty));
                self.insn(Insn::Try {
                    src,
                    dest,
                    result,
                    ok_tag: *ok_tag,
                    fail_tag: *fail_tag,
                    payload: *payload,
                    fail_slot,
                    fail: *fail,
                    cleanup: self.cleanup_slots(0),
                });
            }
            TExprKind::Get {
                vec,
                index,
                elem,
                some_tag,
                none_tag,
            } => {
                let vec = self.read_collection(vec, stable_index(index));
                if self.dead {
                    return;
                }
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                let value = self.alloc(*elem);
                self.insn(Insn::Get {
                    vec,
                    index,
                    value,
                    dest,
                    elem: *elem,
                    some_tag: *some_tag,
                    none_tag: *none_tag,
                });
            }
            TExprKind::Push {
                binding,
                offset,
                value,
                elem,
            } => {
                let value = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Push {
                    vec,
                    offset: *offset,
                    value,
                    elem: *elem,
                });
            }
            TExprKind::CStr(bytes) => self.insn(Insn::CStr {
                dest,
                bytes: bytes.clone(),
            }),
            TExprKind::Extern {
                callee,
                args,
                fixed,
            } => {
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::Extern {
                    dest,
                    name: callee.clone(),
                    args: arg_slots,
                    ret: expr.ty,
                    fixed: *fixed,
                });
            }
            TExprKind::Call { callee, args } => {
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
                }
                if self.in_tail(dest, expr.ty) && !args.iter().any(|arg| arg.ty.is_ref()) {
                    self.terminate(Term::Tail {
                        callee: callee.clone(),
                        args: arg_slots,
                    });
                    return;
                }
                let next = self.fresh();
                self.segs[self.cur].term = Some(Term::Call {
                    callee: callee.clone(),
                    args: arg_slots,
                    dest,
                    next,
                });
                self.cur = next;
                self.dead = false;
            }
            TExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                let cond = self.emit_expr(cond);
                if self.dead {
                    return;
                }
                let then_seg = self.fresh();
                let else_seg = self.fresh();
                let join = self.fresh();
                self.segs[self.cur].term = Some(Term::If {
                    cond,
                    then_seg,
                    else_seg,
                });
                self.dead = true;
                self.cur = then_seg;
                self.dead = false;
                self.emit_block(then_body, Some(dest));
                if !self.dead {
                    self.terminate(Term::Jump(join));
                }
                self.cur = else_seg;
                self.dead = false;
                if let Some(else_body) = else_body {
                    self.emit_into_expr(else_body, dest);
                }
                if !self.dead {
                    self.terminate(Term::Jump(join));
                }
                self.cur = join;
                self.dead = false;
            }
            TExprKind::Block(block) => self.emit_block(block, Some(dest)),
            TExprKind::While { cond, body } => self.emit_while(cond, body),
            TExprKind::ForIter {
                index,
                start,
                end,
                inclusive,
                extract,
                body,
            } => {
                self.emit_for(*index, start, end, *inclusive, extract, body);
            }
            TExprKind::Record { fields } => {
                let mut slots = Vec::new();
                for field in fields {
                    slots.push(self.emit_expr(field));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::Record {
                    dest,
                    fields: slots,
                });
            }
            TExprKind::Field { base, offset } => {
                let src = self.emit_expr(base);
                if self.dead {
                    return;
                }
                self.insn(Insn::Take {
                    dest,
                    src,
                    offset: *offset,
                });
            }
            TExprKind::Ctor { tag, args } => {
                let mut fields = Vec::new();
                for arg in args {
                    let slot = self.emit_expr(arg);
                    if self.dead {
                        return;
                    }
                    fields.push((slot, arg.ty.cells()));
                }
                self.insn(Insn::Make {
                    dest,
                    tag: *tag,
                    fields,
                });
            }
            TExprKind::Match { scrutinee, arms } => self.emit_match(scrutinee, arms, dest),
            TExprKind::Print { value, newline } => {
                let slot = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::Print {
                    slot,
                    ty: value.ty,
                    newline: *newline,
                });
            }
            TExprKind::PrintLit { chars, newline } => {
                self.insn(Insn::PrintLit {
                    chars: chars.clone(),
                    newline: *newline,
                });
            }
            TExprKind::Putc(value) => {
                let slot = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::Putc { slot });
            }
            TExprKind::Cast { expr: inner, to } => {
                if let TExprKind::Int(value) = inner.kind {
                    self.insn(Insn::Const {
                        slot: dest,
                        value: to.bits(value),
                    });
                } else {
                    let src = self.emit_expr(inner);
                    self.insn(Insn::Cast {
                        dest,
                        src,
                        from: inner.ty,
                        to: *to,
                    });
                }
            }
        }
    }

    /// A simple read has no opportunity to mutate its receiver between reading
    /// the binding and consuming it. Other operands still take owning snapshots.
    fn read_collection(&mut self, expr: &TExpr, stable: bool) -> usize {
        if stable {
            if let TExprKind::Name(id, ..) = expr.kind {
                return self.binding_slot[id as usize];
            }
        }
        self.emit_expr(expr)
    }

    pub(super) fn emit_into_expr(&mut self, expr: &TExpr, dest: usize) {
        match &expr.kind {
            TExprKind::Block(block) => self.emit_block(block, Some(dest)),
            _ => self.emit_expr_to(expr, dest),
        }
    }
}

fn stable_index(expr: &TExpr) -> bool {
    match &expr.kind {
        TExprKind::Int(_) | TExprKind::Name(_, ..) => true,
        TExprKind::Unary { expr, .. } | TExprKind::Cast { expr, .. } => stable_index(expr),
        TExprKind::Binary { lhs, rhs, .. } => stable_index(lhs) && stable_index(rhs),
        _ => false,
    }
}
