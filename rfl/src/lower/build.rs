//! Build support for RFL lowering.

use super::*;

struct FnBuild {
    slots: Vec<Slot>,
    next_offset: i64,
    binding_slot: Vec<usize>,
    segs: Vec<Seg>,
    cur: usize,
    dead: bool,
    /// `(continue target, break target)` for each enclosing loop.
    loops: Vec<(usize, usize, usize)>,
    scopes: Vec<Vec<usize>>,
    resources: Arc<crate::resources::ResourceTypes>,
    scope_cleanup: Arc<crate::resources::ResourceTypes>,
    result: Option<usize>,
    root: bool,
    /// The expression under construction is in tail position.
    tail_block: bool,
}

pub(super) fn build_fn(
    func: &TFn,
    func_id: usize,
    structs: Arc<Vec<Vec<Ty>>>,
    tuples: Arc<Vec<Vec<Ty>>>,
    collections: Arc<crate::typed::CollectionTypes>,
    resources: Arc<crate::resources::ResourceTypes>,
    scope_cleanup: Arc<crate::resources::ResourceTypes>,
) -> Result<Proc, CompileError> {
    let mut build = FnBuild {
        slots: Vec::new(),
        next_offset: PAYLOAD,
        binding_slot: Vec::new(),
        segs: Vec::new(),
        cur: 0,
        dead: false,
        loops: Vec::new(),
        scopes: Vec::new(),
        resources: resources.clone(),
        scope_cleanup: scope_cleanup.clone(),
        result: None,
        root: func.name == "main" || func.c_export || func.async_result.is_some(),
        tail_block: true,
    };
    let result_slot = if func.ret == Ty::Unit {
        None
    } else {
        Some(build.alloc(func.ret))
    };
    build.result = result_slot;
    let param_count = func.params.len();
    let mut env_at = None;
    for (index, ty) in func.bindings.iter().enumerate() {
        if func.closure && index == param_count {
            env_at = Some(build.next_offset);
            build.next_offset += 2;
        }
        let slot = if func.shared.get(index).copied().unwrap_or(false) {
            build.alloc_shared(*ty)
        } else {
            build.alloc(*ty)
        };
        build.binding_slot.push(slot);
    }
    if func.closure && func.bindings.len() == param_count {
        env_at = Some(build.next_offset);
        build.next_offset += 2;
    }
    let mut captures = Vec::new();
    // Cell 0 of an environment is its refcount. Captures follow it.
    let mut env_offset = 1i64;
    for cap in &func.captures {
        let cells = if cap.shared { 2 } else { cap.ty.cells() };
        captures.push(LowerCap {
            parent: cap.parent,
            transfer: cap.transfer,
            slot: build.binding_slot[cap.local as usize],
            ty: cap.ty,
            cells,
            offset: env_offset,
            shared: cap.shared,
        });
        env_offset += cells;
    }
    build.cur = build.fresh();
    build.scopes.push(
        func.params
            .iter()
            .copied()
            .chain(func.captures.iter().filter(|c| c.transfer).map(|c| c.local))
            .map(|id| build.binding_slot[id as usize])
            .filter(|slot| scope_cleanup.contains(build.slots[*slot].ty))
            .collect(),
    );
    build.emit_block(&func.body, result_slot);
    build.cleanup_to(0);
    if !build.dead && build.segs[build.cur].term.is_none() {
        build.terminate(Term::Return);
    }
    if build.segs.iter().any(|seg| seg.term.is_none()) {
        return Err(CompileError::plain(format!(
            "compiler bug: `{}` has an unfinished block",
            func.name
        )));
    }
    if func.async_result.is_some() && build.segs.len() > u16::MAX as usize {
        return Err(CompileError::plain(
            "async function has too many resume states",
        ));
    }
    Ok(Proc {
        direct: false,
        async_context: func.async_context.map(|id| build.binding_slot[id as usize]),
        async_result: func.async_result,
        name: func.name.clone(),
        ret: func.ret,
        func_id,
        frame_cells: build.next_offset,
        slots: build.slots,
        param_slots: func
            .params
            .iter()
            .map(|id| build.binding_slot[*id as usize])
            .collect(),
        binding_slots: build.binding_slot,
        env_at,
        captures,
        result_slot,
        segs: build.segs,
        structs,
        tuples,
        collections,
        resources,
        c_export: func.c_export,
    })
}

impl FnBuild {
    fn alloc(&mut self, ty: Ty) -> usize {
        let offset = self.next_offset;
        self.next_offset += ty.cells();
        self.slots.push(Slot {
            offset,
            ty,
            shared: false,
        });
        let slot = self.slots.len() - 1;
        self.track_resource(slot);
        slot
    }

    fn track_resource(&mut self, slot: usize) {
        if self.scope_cleanup.contains(self.slots[slot].ty) {
            if let Some(scope) = self.scopes.last_mut() {
                if !scope.contains(&slot) {
                    scope.push(slot);
                }
            }
        }
    }

    fn cleanup_slots(&self, depth: usize) -> Vec<usize> {
        self.scopes[depth..]
            .iter()
            .rev()
            .flat_map(|s| s.iter().rev().copied())
            .collect()
    }
    fn cleanup_to(&mut self, depth: usize) {
        for slot in self.cleanup_slots(depth) {
            self.insn(Insn::Drop { slot });
        }
    }

    fn alloc_shared(&mut self, ty: Ty) -> usize {
        let offset = self.next_offset;
        self.next_offset += 2;
        self.slots.push(Slot {
            offset,
            ty,
            shared: true,
        });
        self.slots.len() - 1
    }

    fn fresh(&mut self) -> usize {
        let id = self.segs.len();
        self.segs.push(Seg {
            insns: Vec::new(),
            term: None,
        });
        id
    }

    fn insn(&mut self, insn: Insn) {
        if self.dead {
            return;
        }
        self.segs[self.cur].insns.push(insn);
    }

    fn terminate(&mut self, term: Term) {
        if self.dead {
            return;
        }
        self.segs[self.cur].term = Some(term);
        self.dead = true;
    }

    fn emit_block(&mut self, block: &TBlock, result: Option<usize>) {
        let tail_block = std::mem::replace(&mut self.tail_block, false);
        let depth = self.scopes.len();
        self.scopes.push(vec![]);
        for stmt in &block.stmts {
            if self.dead {
                break;
            }
            self.emit_stmt(stmt, result);
        }
        if !self.dead {
            if let Some(tail) = &block.tail {
                self.tail_block = tail_block;
                // Even a unit-valued tail is in tail position. The ordinary
                // operand helper deliberately suppresses tail calls.
                let dest = result.unwrap_or_else(|| self.alloc(tail.ty));
                self.emit_expr_to(tail, dest);
            }
        }
        // A sibling branch inherits its own tail context, irrespective of
        // whether this branch returned or constructed a nested block.
        if result.is_some_and(|slot| self.slots[slot].ty.is_ref()) {
            // An implicit borrowed receiver is represented as a block holding
            // its snapshot and returning a reference to it. Its owner must
            // survive the enclosing call, rather than die at this inner block.
            // Conservatively extend it to the enclosing lexical scope.
            let owners = self.scopes.pop().unwrap();
            self.scopes.last_mut().unwrap().extend(owners);
        } else {
            self.cleanup_to(depth);
            self.scopes.pop();
        }
        self.tail_block = tail_block;
    }

    fn emit_stmt(&mut self, stmt: &TStmt, _result: Option<usize>) {
        match stmt {
            TStmt::Let { binding, init } => {
                let dest = self.binding_slot[*binding as usize];
                if self.slots[dest].shared {
                    self.insn(Insn::BoxNew { slot: dest });
                }
                self.emit_expr_to(init, dest);
                self.track_resource(dest);
            }
            TStmt::Destructure { pat, init } => {
                let slot = if let TExprKind::Name(id, 0) = init.kind {
                    if self.resources.contains(init.ty) {
                        let dest = self.alloc(init.ty);
                        self.insn(Insn::Move {
                            dest,
                            src: self.binding_slot[id as usize],
                        });
                        dest
                    } else {
                        self.emit_expr(init)
                    }
                } else {
                    self.emit_expr(init)
                };
                if self.dead {
                    return;
                }
                let fail = self.fresh();
                self.bind_pat(pat, slot, fail);
                self.insn(Insn::Drop { slot });
                let cont = self.cur;
                self.cur = fail;
                self.dead = false;
                self.terminate(Term::Fail);
                self.cur = cont;
                self.dead = false;
            }
            TStmt::Assign { binding, value } => {
                let dest = self.binding_slot[*binding as usize];
                if self.slots[dest].ty.owns_buffer() {
                    let transfer = !matches!(value.kind, TExprKind::Name(_, ..));
                    let src = self.emit_expr(value);
                    if src != dest {
                        self.insn(Insn::VecStore {
                            dest,
                            src,
                            transfer,
                        });
                    }
                } else {
                    self.emit_expr_to(value, dest);
                }
            }
            TStmt::PtrStore {
                ptr,
                index,
                value,
                stride,
            } => {
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
                let src = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let cells = self.slots[src].ty.cells();
                self.insn(Insn::PtrStore {
                    ptr,
                    index,
                    src,
                    stride: *stride,
                    cells,
                });
            }
            TStmt::FieldAssign {
                binding,
                offset,
                value,
            } => {
                let dest = self.binding_slot[*binding as usize];
                let src = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::FieldStore {
                    dest,
                    offset: *offset,
                    src,
                });
            }
            TStmt::IndexAssign {
                binding,
                offset,
                index,
                value,
                elem,
                container,
            } => {
                let vec = self.binding_slot[*binding as usize];
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                let value = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::IndexStore {
                    vec,
                    index,
                    value,
                    elem: *elem,
                    offset: *offset,
                    container: *container,
                });
            }
            TStmt::Expr(expr) => {
                let slot = self.emit_expr(expr);
                self.insn(Insn::Drop { slot });
            }
            TStmt::Return(value) => {
                let saved = self.tail_block;
                self.tail_block = true;
                // `result` is only where this block's tail is stored. A `return`
                // leaves the function, so the value belongs in the function slot
                // that `emit_return` copies back to the caller.
                if let Some(value) = value {
                    if let Some(slot) = self.result {
                        self.emit_expr_to(value, slot);
                    } else {
                        let dest = self.alloc(value.ty);
                        self.emit_expr_to(value, dest);
                    }
                }
                self.tail_block = saved;
                if !self.dead {
                    self.cleanup_to(0);
                    self.terminate(Term::Return);
                }
            }
            TStmt::Break => self.jump_loop(false),
            TStmt::Continue => self.jump_loop(true),
        }
    }

    fn jump_loop(&mut self, to_head: bool) {
        let Some(&(head, after, depth)) = self.loops.last() else {
            return;
        };
        self.cleanup_to(depth);
        self.terminate(Term::Jump(if to_head { head } else { after }));
    }

    fn emit_while(&mut self, cond: &TExpr, body: &TBlock) {
        let saved = self.tail_block;
        self.tail_block = false;
        let head = self.fresh();
        let body_seg = self.fresh();
        let after = self.fresh();
        self.terminate(Term::Jump(head));
        self.cur = head;
        self.dead = false;
        let cond_slot = self.emit_expr(cond);
        if !self.dead {
            self.segs[self.cur].term = Some(Term::If {
                cond: cond_slot,
                then_seg: body_seg,
                else_seg: after,
            });
            self.dead = true;
        }
        self.loops.push((head, after, self.scopes.len()));
        self.cur = body_seg;
        self.dead = false;
        self.emit_block(body, None);
        if !self.dead {
            self.terminate(Term::Jump(head));
        }
        self.loops.pop();
        self.cur = after;
        self.dead = false;
        self.tail_block = saved;
    }

    fn emit_for(
        &mut self,
        index: u32,
        start: &TExpr,
        end: &TExpr,
        inclusive: bool,
        extract: &Option<Box<TStmt>>,
        body: &TBlock,
    ) {
        if extract.is_none() && !inclusive && self.fold_vector_fill(start, end, body) {
            return;
        }
        let saved = self.tail_block;
        self.tail_block = false;
        let idx = self.binding_slot[index as usize];
        self.emit_expr_to(start, idx);
        if self.dead {
            self.tail_block = saved;
            return;
        }
        let end_slot = self.emit_expr(end);
        if self.dead {
            self.tail_block = saved;
            return;
        }
        let head = self.fresh();
        let body_seg = self.fresh();
        let step = self.fresh();
        let after = self.fresh();
        self.terminate(Term::Jump(head));
        self.cur = head;
        self.dead = false;
        let cond = self.alloc(Ty::Bool);
        let op = if inclusive { BinOp::Le } else { BinOp::Lt };
        self.insn(Insn::Bin {
            dest: cond,
            op,
            lhs: idx,
            rhs: end_slot,
        });
        self.terminate(Term::If {
            cond,
            then_seg: body_seg,
            else_seg: after,
        });
        self.loops.push((step, after, self.scopes.len()));
        self.cur = body_seg;
        self.dead = false;
        if let Some(extract) = extract {
            self.emit_stmt(extract, None);
        }
        if !self.dead {
            self.emit_block(body, None);
        }
        if !self.dead {
            self.terminate(Term::Jump(step));
        }
        self.cur = step;
        self.dead = false;
        if inclusive {
            let eq = self.alloc(Ty::Bool);
            self.insn(Insn::Bin {
                dest: eq,
                op: BinOp::Eq,
                lhs: idx,
                rhs: end_slot,
            });
            let inc = self.fresh();
            self.terminate(Term::If {
                cond: eq,
                then_seg: after,
                else_seg: inc,
            });
            self.cur = inc;
            self.dead = false;
        }
        let one = self.alloc(self.slots[idx].ty);
        self.insn(Insn::Const {
            slot: one,
            value: 1,
        });
        self.insn(Insn::Bin {
            dest: idx,
            op: BinOp::Add,
            lhs: idx,
            rhs: one,
        });
        if !self.dead {
            self.terminate(Term::Jump(head));
        }
        self.loops.pop();
        self.cur = after;
        self.dead = false;
        self.tail_block = saved;
    }

    fn emit_expr(&mut self, expr: &TExpr) -> usize {
        if self.dead {
            return 0;
        }
        // An operand is evaluated now, even if a later operand mutates its
        // source binding. Keep a value snapshot, including its managed share.
        // Operands are never in tail position: their consumer still has work.
        let tail = std::mem::replace(&mut self.tail_block, false);
        let dest = self.alloc(expr.ty);
        self.emit_expr_to(expr, dest);
        self.tail_block = tail;
        dest
    }

    fn emit_logic(&mut self, op: BinOp, lhs: &TExpr, rhs: &TExpr, dest: usize) {
        let cond = self.emit_expr(lhs);
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
        self.cur = then_seg;
        self.dead = false;
        if op == BinOp::And {
            self.emit_expr_to(rhs, dest);
        } else {
            self.insn(Insn::Const {
                slot: dest,
                value: 1,
            });
        }
        if !self.dead {
            self.terminate(Term::Jump(join));
        }
        self.cur = else_seg;
        self.dead = false;
        if op == BinOp::And {
            self.insn(Insn::Const {
                slot: dest,
                value: 0,
            });
        } else {
            self.emit_expr_to(rhs, dest);
        }
        if !self.dead {
            self.terminate(Term::Jump(join));
        }
        self.cur = join;
        self.dead = false;
    }

    fn emit_match(&mut self, scrutinee: &TExpr, arms: &[crate::typed::TArm], dest: usize) {
        let slot = self.emit_expr(scrutinee);
        if self.dead {
            return;
        }
        let join = self.fresh();
        for arm in arms {
            let depth = self.scopes.len();
            self.scopes.push(vec![]);
            let fail = self.fresh();
            self.bind_pat(&arm.pat, slot, fail);
            if !self.dead {
                if let Some(guard) = &arm.guard {
                    let cond = self.emit_expr(guard);
                    if !self.dead {
                        let ok = self.fresh();
                        self.terminate(Term::If {
                            cond,
                            then_seg: ok,
                            else_seg: fail,
                        });
                        self.cur = ok;
                        self.dead = false;
                    }
                }
            }
            if !self.dead {
                // Bindings now retain the selected fields. End the consuming
                // snapshot before user code, so wildcard resources drop here.
                self.insn(Insn::Drop { slot });
                self.emit_block(&arm.body, Some(dest));
            }
            self.cleanup_to(depth);
            if !self.dead {
                self.terminate(Term::Jump(join));
            }
            self.cur = fail;
            self.dead = false;
            self.cleanup_to(depth);
            self.scopes.pop();
        }
        self.terminate(Term::Fail);
        self.cur = join;
        self.dead = false;
    }

    fn bind_pat(&mut self, pat: &crate::typed::TPat, slot: usize, fail: usize) {
        if self.dead {
            return;
        }
        match pat {
            crate::typed::TPat::Wild => {}
            crate::typed::TPat::Bind(id) => {
                let dest = self.binding_slot[*id as usize];
                if dest != slot {
                    self.insn(Insn::Copy { dest, src: slot });
                }
                self.track_resource(dest);
            }
            crate::typed::TPat::Ctor { tag, fields } => {
                let ok = self.fresh();
                self.terminate(Term::Tag {
                    slot,
                    tag: *tag,
                    then_seg: ok,
                    else_seg: fail,
                });
                self.cur = ok;
                self.dead = false;
                // Payload objects are refcount, tag, then fields.
                let mut offset = 2i64;
                for field in fields {
                    let cells = field.ty.cells();
                    match field.pat.as_ref() {
                        crate::typed::TPat::Wild => {}
                        crate::typed::TPat::Bind(id) => {
                            let dest = self.binding_slot[*id as usize];
                            self.insn(Insn::Read {
                                dest,
                                src: slot,
                                offset,
                                cells,
                            });
                            self.track_resource(dest);
                        }
                        nested => {
                            let dest = self.alloc(field.ty);
                            self.insn(Insn::Read {
                                dest,
                                src: slot,
                                offset,
                                cells,
                            });
                            self.bind_pat(nested, dest, fail);
                            self.insn(Insn::Drop { slot: dest });
                            if self.dead {
                                return;
                            }
                        }
                    }
                    offset += cells;
                }
            }
            crate::typed::TPat::Lit(value) => {
                let ok = self.fresh();
                self.terminate(Term::Lit {
                    slot,
                    value: *value,
                    then_seg: ok,
                    else_seg: fail,
                });
                self.cur = ok;
                self.dead = false;
            }
            crate::typed::TPat::Product { fields } => {
                for field in fields {
                    match field.pat.as_ref() {
                        crate::typed::TPat::Wild => {}
                        crate::typed::TPat::Bind(id) => {
                            let dest = self.binding_slot[*id as usize];
                            self.insn(Insn::Take {
                                dest,
                                src: slot,
                                offset: field.offset,
                            });
                            self.track_resource(dest);
                        }
                        nested => {
                            let dest = self.alloc(field.ty);
                            self.insn(Insn::Take {
                                dest,
                                src: slot,
                                offset: field.offset,
                            });
                            self.bind_pat(nested, dest, fail);
                            self.insn(Insn::Drop { slot: dest });
                            if self.dead {
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    fn in_tail(&self, dest: usize, ty: Ty) -> bool {
        if !self.tail_block || self.root || self.scopes.iter().any(|s| !s.is_empty()) {
            return false;
        }
        match self.result {
            Some(slot) => dest == slot,
            None => ty == Ty::Unit,
        }
    }
}

pub(super) fn fold_bin(op: BinOp, result: Ty, operand: Ty, a: i64, b: i64) -> Option<i64> {
    let value = match op {
        BinOp::Add => a.wrapping_add(b),
        BinOp::Sub => a.wrapping_sub(b),
        BinOp::Mul => a.wrapping_mul(b),
        BinOp::Div if b == 0 => return None,
        BinOp::Rem if b == 0 => return None,
        BinOp::Div => a.wrapping_div(b),
        BinOp::Rem => a.wrapping_rem(b),
        BinOp::Eq => i64::from(bits(operand, a) == bits(operand, b)),
        BinOp::Ne => i64::from(bits(operand, a) != bits(operand, b)),
        BinOp::Lt => i64::from(less(operand, a, b)),
        BinOp::Le => i64::from(!less(operand, b, a)),
        BinOp::Gt => i64::from(less(operand, b, a)),
        BinOp::Ge => i64::from(!less(operand, a, b)),
        BinOp::And | BinOp::Or => return None,
        BinOp::BitAnd | BinOp::BitOr | BinOp::BitXor | BinOp::Shl | BinOp::Shr => {
            fold_bitwise(operand, op, a, b)
        }
    };
    Some(if op.is_compare() {
        value
    } else {
        result.bits(value)
    })
}

pub(super) fn bits(ty: Ty, value: i64) -> u64 {
    match ty {
        Ty::I8 | Ty::U8 => value as u8 as u64,
        Ty::I16 | Ty::U16 => value as u16 as u64,
        Ty::I32 | Ty::U32 | Ty::Usize => value as u32 as u64,
        _ => value as u64,
    }
}

pub(super) fn less(ty: Ty, a: i64, b: i64) -> bool {
    if ty.is_signed() {
        a < b
    } else {
        bits(ty, a) < bits(ty, b)
    }
}

#[path = "expressions.rs"]
mod expr;

#[path = "vector_fill_build.rs"]
mod vector_fill_build;
