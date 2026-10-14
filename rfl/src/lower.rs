//! Lower checked RFL to Ripple IR.
//!
//! Each RFL function is `i16* rfl_fn_*(i16* frame)`. It returns the next
//! activation record, or null when `main` is finished. Records live on the
//! heap except the root `main` record, which the driver allocates once.
//! `rfl_frame_alloc` bumps a chunk from `malloc`. Returning rewinds that bump,
//! so a call reclaims its record before the caller continues. The hardware
//! stack only holds the driver and the function that is running right now.

use rcc_ir::ir::{FatPointer, Function, GlobalVariable, IrBinaryOp, IrBuilder, IrType, IrUnaryOp, Linkage, Module, Value};
use rcc_ir::BankTag;

use crate::ast::{BinOp, UnaryOp};
use crate::error::CompileError;
use crate::ty::Ty;
use crate::typed::{AdtDef, CtorDef, TBlock, TExpr, TExprKind, TFn, TModule, TStmt};

const RESUME: i64 = 0;
const FUNC_ID: i64 = 1;
const CALLER: i64 = 2;
const DEST: i64 = 4;
const PAYLOAD: i64 = 5;

struct Slot {
    offset: i64,
    ty: Ty,
}

struct Proc {
    name: String,
    ret: Ty,
    func_id: usize,
    frame_cells: i64,
    slots: Vec<Slot>,
    param_slots: Vec<usize>,
    /// Slot index of each binding, in binding order.
    binding_slots: Vec<usize>,
    /// Frame offset of the environment pointer. Present on every function.
    env_at: Option<i64>,
    captures: Vec<LowerCap>,
    result_slot: Option<usize>,
    segs: Vec<Seg>,
    structs: Vec<Vec<Ty>>,
    tuples: Vec<Vec<Ty>>,
}

#[derive(Clone)]
struct LowerCap {
    /// Binding in the function that creates the closure.
    parent: u32,
    /// Slot in this function that receives the copy.
    slot: usize,
    ty: Ty,
    cells: i64,
    /// Cell offset inside the environment object. Cell 0 is the refcount.
    offset: i64,
}

struct Seg {
    insns: Vec<Insn>,
    term: Option<Term>,
}

enum Insn {
    Const { slot: usize, value: i64 },
    Copy { dest: usize, src: usize },
    Bin { dest: usize, op: BinOp, lhs: usize, rhs: usize },
    Neg { dest: usize, src: usize },
    Not { dest: usize, src: usize },
    Cast { dest: usize, src: usize, from: Ty, to: Ty },
    /// Build a constructor into `dest`. Empty `fields` is an immediate nullary value.
    Make { dest: usize, tag: u16, fields: Vec<(usize, i64)> },
    /// Copy `cells` words from a payload object into `dest`.
    Read { dest: usize, src: usize, offset: i64, cells: i64 },
    /// Decimal integer or `true`/`false`, optionally followed by a newline.
    Print { slot: usize, ty: Ty, newline: bool },
    PrintLit { chars: Vec<u16>, newline: bool },
    Putc { slot: usize },
    /// Build a closure value for a lifted lambda.
    Closure { dest: usize, func: String },
    VecLit { dest: usize, elems: Vec<usize>, elem: Ty },
    ArrayLit { dest: usize, elems: Vec<usize>, elem: Ty },
    /// `base[start..end]`. `inclusive` means the end index is kept.
    SliceNew { dest: usize, base: usize, start: usize, end: usize, inclusive: bool, elem: Ty },
    ToVec { dest: usize, src: usize, elem: Ty },
    Index { dest: usize, vec: usize, index: usize, elem: Ty },
    Len { dest: usize, vec: usize },
    Empty { dest: usize, vec: usize },
    Clear { vec: usize },
    Pop { vec: usize, value: usize, dest: usize, elem: Ty, some_tag: u16, none_tag: u16 },
    Get { vec: usize, index: usize, value: usize, dest: usize, elem: Ty, some_tag: u16, none_tag: u16 },
    Push { vec: usize, value: usize, elem: Ty },
    IndexStore { vec: usize, index: usize, value: usize, elem: Ty },
    /// Address of a local slot. The destination holds that pointer.
    Borrow { dest: usize, slot: usize },
    /// `expr?`. Failure writes the function result and returns.
    Try {
        src: usize,
        dest: usize,
        result: usize,
        ok_tag: u16,
        fail_tag: u16,
        payload: Ty,
        fail_slot: Option<usize>,
        fail: Option<Ty>,
    },
    /// Release `dest`, then put `src` there. `transfer` clears `src` without an extra retain.
    VecStore { dest: usize, src: usize, transfer: bool },
    /// Store fields of an inline struct or tuple, in order.
    Record { dest: usize, fields: Vec<usize> },
    /// Copy `cells` words from `src` at `offset` into `dest`.
    Take { dest: usize, src: usize, offset: i64 },
    /// Store `src` at a cell offset inside `dest`.
    FieldStore { dest: usize, offset: i64, src: usize },
}

enum Term {
    Return,
    Jump(usize),
    If { cond: usize, then_seg: usize, else_seg: usize },
    Call { callee: String, args: Vec<usize>, dest: usize, next: usize },
    /// Call the closure in `slot`. `arg_tys` selects where each argument is stored.
    Apply { slot: usize, args: Vec<usize>, arg_tys: Vec<Ty>, dest: usize, next: usize },
    Tag { slot: usize, tag: u16, then_seg: usize, else_seg: usize },
    Fail,
    /// Direct tail call. The callee reuses this frame's caller link.
    Tail { callee: String, args: Vec<usize> },
    /// Compare `slot` with `value` and branch.
    Lit { slot: usize, value: i64, then_seg: usize, else_seg: usize },
}

struct Built {
    procs: Vec<Proc>,
    adts: Vec<AdtDef>,
    structs: Vec<Vec<Ty>>,
    tuples: Vec<Vec<Ty>>,
    need_alloc: bool,
    need_fail: bool,
    need_heap: bool,
    need_io: bool,
    need_print_num: bool,
}

pub fn lower(module_name: &str, module: &TModule) -> Result<Module, CompileError> {
    let built = build(module)?;
    emit(module_name, &built)
}

fn build(module: &TModule) -> Result<Built, CompileError> {
    let mut procs = Vec::new();
    let mut need_alloc = false;
    let mut need_fail = false;
    let mut need_heap = false;
    let mut need_io = false;
    let mut need_print_num = false;
    for (func_id, func) in module.functions.iter().enumerate() {
        let proc = build_fn(func, func_id, module.structs.clone(), module.tuples.clone())?;
        if proc.segs.iter().any(|seg| matches!(seg.term, Some(Term::Call { .. } | Term::Apply { .. } | Term::Tail { .. }))) {
            need_alloc = true;
        }
        if proc.segs.len() > 1 || proc.segs.iter().any(|seg| matches!(seg.term, Some(Term::Fail))) {
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| seg.insns.iter().any(|insn| matches!(insn, Insn::Bin { op: BinOp::Div | BinOp::Rem, .. }))) {
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| seg.insns.iter().any(|insn| matches!(insn, Insn::Make { fields, .. } if !fields.is_empty()))) {
            need_heap = true;
            need_fail = true;
        }
        if !proc.captures.is_empty() {
            need_heap = true;
            need_fail = true;
        }
        if proc.slots.iter().any(|slot| slot.ty.owns_buffer()) {
            need_heap = true;
            need_fail = true;
        }
        if proc.slots.iter().any(|slot| matches!(slot.ty, Ty::Adt(_) | Ty::Func(_))) {
            need_heap = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(|insn| {
                matches!(
                    insn,
                    Insn::Pop { .. }
                        | Insn::Get { .. }
                        | Insn::Push { .. }
                        | Insn::Clear { .. }
                        | Insn::IndexStore { .. }
                        | Insn::Index { .. }
                        | Insn::VecLit { .. }
                        | Insn::SliceNew { .. }
                        | Insn::ToVec { .. }
                        | Insn::Try { fail: Some(_), .. }
                )
            })
        }) {
            need_heap = true;
            need_fail = true;
        }
        for seg in &proc.segs {
            for insn in &seg.insns {
                match insn {
                    Insn::Print { ty, .. } => {
                        need_io = true;
                        if ty.is_int() {
                            need_print_num = true;
                        }
                    }
                    Insn::PrintLit { .. } | Insn::Putc { .. } => need_io = true,
                    _ => {}
                }
            }
        }
        procs.push(proc);
    }
    if !procs.iter().any(|proc| proc.name == "main") {
        return Err(CompileError::plain("program has no main"));
    }
    Ok(Built {
        procs,
        adts: module.adts.clone(),
        structs: module.structs.clone(),
        tuples: module.tuples.clone(),
        need_alloc,
        need_fail,
        need_heap,
        need_io,
        need_print_num,
    })
}

struct FnBuild {
    slots: Vec<Slot>,
    next_offset: i64,
    binding_slot: Vec<usize>,
    segs: Vec<Seg>,
    cur: usize,
    dead: bool,
    /// `(continue target, break target)` for each enclosing loop.
    loops: Vec<(usize, usize)>,
    result: Option<usize>,
    fn_name: String,
    /// The expression under construction is in tail position.
    tail_block: bool,
}

fn build_fn(func: &TFn, func_id: usize, structs: Vec<Vec<Ty>>, tuples: Vec<Vec<Ty>>) -> Result<Proc, CompileError> {
    let mut build = FnBuild {
        slots: Vec::new(),
        next_offset: PAYLOAD,
        binding_slot: Vec::new(),
        segs: Vec::new(),
        cur: 0,
        dead: false,
        loops: Vec::new(),
        result: None,
        fn_name: func.name.clone(),
        tail_block: true,
    };
    let result_slot = if func.ret == Ty::Unit { None } else { Some(build.alloc(func.ret)) };
    build.result = result_slot;
    let param_count = func.params.len();
    let mut env_at = None;
    for (index, ty) in func.bindings.iter().enumerate() {
        if func.closure && index == param_count {
            env_at = Some(build.next_offset);
            build.next_offset += 2;
        }
        let slot = build.alloc(*ty);
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
        let cells = cap.ty.cells();
        captures.push(LowerCap {
            parent: cap.parent,
            slot: build.binding_slot[cap.local as usize],
            ty: cap.ty,
            cells,
            offset: env_offset,
        });
        env_offset += cells;
    }
    build.cur = build.fresh();
    build.emit_block(&func.body, result_slot);
    if !build.dead && build.segs[build.cur].term.is_none() {
        build.terminate(Term::Return);
    }
    if build.segs.iter().any(|seg| seg.term.is_none()) {
        return Err(CompileError::plain(format!("compiler bug: `{}` has an unfinished block", func.name)));
    }
    Ok(Proc {
        name: func.name.clone(),
        ret: func.ret,
        func_id,
        frame_cells: build.next_offset,
        slots: build.slots,
        param_slots: func.params.iter().map(|id| build.binding_slot[*id as usize]).collect(),
        binding_slots: build.binding_slot,
        env_at,
        captures,
        result_slot,
        segs: build.segs,
        structs,
        tuples,
    })
}

impl FnBuild {
    fn alloc(&mut self, ty: Ty) -> usize {
        let offset = self.next_offset;
        self.next_offset += ty.cells();
        self.slots.push(Slot { offset, ty });
        self.slots.len() - 1
    }

    fn fresh(&mut self) -> usize {
        let id = self.segs.len();
        self.segs.push(Seg { insns: Vec::new(), term: None });
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
        for stmt in &block.stmts {
            if self.dead {
                return;
            }
            self.emit_stmt(stmt, result);
        }
        if self.dead {
            return;
        }
        if let Some(tail) = &block.tail {
            self.tail_block = tail_block;
            if let Some(result) = result {
                self.emit_expr_to(tail, result);
            } else {
                self.emit_expr(tail);
            }
            self.tail_block = false;
        }
    }

    fn emit_stmt(&mut self, stmt: &TStmt, result: Option<usize>) {
        match stmt {
            TStmt::Let { binding, init } => {
                let dest = self.binding_slot[*binding as usize];
                self.emit_expr_to(init, dest);
            }
            TStmt::Destructure { pat, init } => {
                let slot = self.emit_expr(init);
                if self.dead {
                    return;
                }
                let fail = self.fresh();
                self.bind_pat(pat, slot, fail);
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
                    let transfer = !matches!(value.kind, TExprKind::Name(_));
                    let src = self.emit_expr(value);
                    if src != dest {
                        self.insn(Insn::VecStore { dest, src, transfer });
                    }
                } else {
                    self.emit_expr_to(value, dest);
                }
            }
            TStmt::FieldAssign { binding, offset, value } => {
                let dest = self.binding_slot[*binding as usize];
                let src = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::FieldStore { dest, offset: *offset, src });
            }
            TStmt::IndexAssign { binding, index, value, elem } => {
                let vec = self.binding_slot[*binding as usize];
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                let value = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::IndexStore { vec, index, value, elem: *elem });
            }
            TStmt::Expr(expr) => {
                self.emit_expr(expr);
            }
            TStmt::Return(value) => {
                let saved = self.tail_block;
                self.tail_block = true;
                if let (Some(result), Some(value)) = (result, value) {
                    self.emit_expr_to(value, result);
                } else if let Some(value) = value {
                    self.emit_expr(value);
                }
                self.tail_block = saved;
                if !self.dead {
                    self.terminate(Term::Return);
                }
            }
            TStmt::Break => self.jump_loop(false),
            TStmt::Continue => self.jump_loop(true),
        }
    }

    fn jump_loop(&mut self, to_head: bool) {
        let Some(&(head, after)) = self.loops.last() else {
            return;
        };
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
            self.segs[self.cur].term = Some(Term::If { cond: cond_slot, then_seg: body_seg, else_seg: after });
            self.dead = true;
        }
        self.loops.push((head, after));
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
        self.insn(Insn::Bin { dest: cond, op, lhs: idx, rhs: end_slot });
        self.terminate(Term::If { cond, then_seg: body_seg, else_seg: after });
        self.loops.push((step, after));
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
            self.insn(Insn::Bin { dest: eq, op: BinOp::Eq, lhs: idx, rhs: end_slot });
            let inc = self.fresh();
            self.terminate(Term::If { cond: eq, then_seg: after, else_seg: inc });
            self.cur = inc;
            self.dead = false;
        }
        let one = self.alloc(self.slots[idx].ty);
        self.insn(Insn::Const { slot: one, value: 1 });
        self.insn(Insn::Bin { dest: idx, op: BinOp::Add, lhs: idx, rhs: one });
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
        if let TExprKind::Name(id) = expr.kind {
            return self.binding_slot[id as usize];
        }
        let dest = self.alloc(expr.ty);
        self.emit_expr_to(expr, dest);
        dest
    }

    fn emit_expr_to(&mut self, expr: &TExpr, dest: usize) {
        if self.dead {
            return;
        }
        match &expr.kind {
            TExprKind::Int(value) => self.insn(Insn::Const { slot: dest, value: expr.ty.bits(*value) }),
            TExprKind::Bool(value) => self.insn(Insn::Const { slot: dest, value: i64::from(*value) }),
            TExprKind::Name(id) => {
                let src = self.binding_slot[*id as usize];
                if src != dest {
                    self.insn(Insn::Copy { dest, src });
                }
            }
            TExprKind::Unary { op: UnaryOp::Neg, expr: inner } => {
                if let TExprKind::Int(value) = inner.kind {
                    self.insn(Insn::Const { slot: dest, value: expr.ty.bits(value) });
                } else {
                    let src = self.emit_expr(inner);
                    self.insn(Insn::Neg { dest, src });
                }
            }
            TExprKind::Unary { op: UnaryOp::Not, expr: inner } => {
                let src = self.emit_expr(inner);
                self.insn(Insn::Not { dest, src });
            }
            TExprKind::Binary { op, lhs, rhs } if matches!(op, BinOp::And | BinOp::Or) => {
                self.emit_logic(*op, lhs, rhs, dest);
            }
            TExprKind::Binary { op, lhs, rhs } => {
                if let (TExprKind::Int(a), TExprKind::Int(b)) = (&lhs.kind, &rhs.kind) {
                    match fold_bin(*op, expr.ty, lhs.ty, *a, *b) {
                        Some(value) => self.insn(Insn::Const { slot: dest, value: expr.ty.bits(value) }),
                        None => self.terminate(Term::Fail),
                    }
                    return;
                }
                let left = self.emit_expr(lhs);
                let right = self.emit_expr(rhs);
                self.insn(Insn::Bin { dest, op: *op, lhs: left, rhs: right });
            }
            TExprKind::Apply { callee, args, params } => {
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
            TExprKind::Closure { func } => self.insn(Insn::Closure { dest, func: func.clone() }),
            TExprKind::ArrayLit { elems, elem } => {
                let mut slots = Vec::new();
                for elem in elems {
                    slots.push(self.emit_expr(elem));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::ArrayLit { dest, elems: slots, elem: *elem });
            }
            TExprKind::Slice { base, start, end, inclusive, elem } => {
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
                self.insn(Insn::SliceNew { dest, base, start, end, inclusive: *inclusive, elem: *elem });
            }
            TExprKind::ToVec { src, elem } => {
                let src = self.emit_expr(src);
                if self.dead {
                    return;
                }
                self.insn(Insn::ToVec { dest, src, elem: *elem });
            }
            TExprKind::VecLit { elems, elem } => {
                let mut slots = Vec::new();
                for elem in elems {
                    slots.push(self.emit_expr(elem));
                    if self.dead {
                        return;
                    }
                }
                self.insn(Insn::VecLit { dest, elems: slots, elem: *elem });
            }
            TExprKind::Index { base, index, elem } => {
                let vec = self.emit_expr(base);
                if self.dead {
                    return;
                }
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                self.insn(Insn::Index { dest, vec, index, elem: *elem });
            }
            TExprKind::Len { vec } => {
                let vec = self.emit_expr(vec);
                if self.dead {
                    return;
                }
                self.insn(Insn::Len { dest, vec });
            }
            TExprKind::Empty { vec } => {
                let vec = self.emit_expr(vec);
                if self.dead {
                    return;
                }
                self.insn(Insn::Empty { dest, vec });
            }
            TExprKind::Clear { binding } => {
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Clear { vec });
            }
            TExprKind::Pop { binding, elem, some_tag, none_tag } => {
                let value = self.alloc(*elem);
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Pop { vec, value, dest, elem: *elem, some_tag: *some_tag, none_tag: *none_tag });
            }
            TExprKind::Borrow { binding } => {
                let slot = self.binding_slot[*binding as usize];
                self.insn(Insn::Borrow { dest, slot });
            }
            TExprKind::Try { expr, ok_tag, fail_tag, payload, fail } => {
                let src = self.emit_expr(expr);
                if self.dead {
                    return;
                }
                let result = self.result.expect("? is used in a function that returns a value");
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
                });
            }
            TExprKind::Get { vec, index, elem, some_tag, none_tag } => {
                let vec = self.emit_expr(vec);
                if self.dead {
                    return;
                }
                let index = self.emit_expr(index);
                if self.dead {
                    return;
                }
                let value = self.alloc(*elem);
                self.insn(Insn::Get { vec, index, value, dest, elem: *elem, some_tag: *some_tag, none_tag: *none_tag });
            }
            TExprKind::Push { binding, value, elem } => {
                let value = self.emit_expr(value);
                if self.dead {
                    return;
                }
                let vec = self.binding_slot[*binding as usize];
                self.insn(Insn::Push { vec, value, elem: *elem });
            }
            TExprKind::Call { callee, args } => {
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
                }
                if self.in_tail(dest, expr.ty) {
                    self.terminate(Term::Tail { callee: callee.clone(), args: arg_slots });
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
            TExprKind::If { cond, then_body, else_body } => {
                let cond = self.emit_expr(cond);
                if self.dead {
                    return;
                }
                let then_seg = self.fresh();
                let else_seg = self.fresh();
                let join = self.fresh();
                self.segs[self.cur].term = Some(Term::If { cond, then_seg, else_seg });
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
            TExprKind::ForIter { index, start, end, inclusive, extract, body } => {
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
                self.insn(Insn::Record { dest, fields: slots });
            }
            TExprKind::Field { base, offset } => {
                let src = self.emit_expr(base);
                if self.dead {
                    return;
                }
                self.insn(Insn::Take { dest, src, offset: *offset });
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
                self.insn(Insn::Make { dest, tag: *tag, fields });
            }
            TExprKind::Match { scrutinee, arms } => self.emit_match(scrutinee, arms, dest),
            TExprKind::Print { value, newline } => {
                let slot = self.emit_expr(value);
                if self.dead {
                    return;
                }
                self.insn(Insn::Print { slot, ty: value.ty, newline: *newline });
            }
            TExprKind::PrintLit { chars, newline } => {
                self.insn(Insn::PrintLit { chars: chars.clone(), newline: *newline });
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
                    self.insn(Insn::Const { slot: dest, value: to.bits(value) });
                } else {
                    let src = self.emit_expr(inner);
                    self.insn(Insn::Cast { dest, src, from: inner.ty, to: *to });
                }
            }
        }
    }

    fn emit_into_expr(&mut self, expr: &TExpr, dest: usize) {
        match &expr.kind {
            TExprKind::Block(block) => self.emit_block(block, Some(dest)),
            _ => self.emit_expr_to(expr, dest),
        }
    }

    fn emit_logic(&mut self, op: BinOp, lhs: &TExpr, rhs: &TExpr, dest: usize) {
        let cond = self.emit_expr(lhs);
        if self.dead {
            return;
        }
        let then_seg = self.fresh();
        let else_seg = self.fresh();
        let join = self.fresh();
        self.segs[self.cur].term = Some(Term::If { cond, then_seg, else_seg });
        self.cur = then_seg;
        self.dead = false;
        if op == BinOp::And {
            self.emit_expr_to(rhs, dest);
        } else {
            self.insn(Insn::Const { slot: dest, value: 1 });
        }
        if !self.dead {
            self.terminate(Term::Jump(join));
        }
        self.cur = else_seg;
        self.dead = false;
        if op == BinOp::And {
            self.insn(Insn::Const { slot: dest, value: 0 });
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
            let fail = self.fresh();
            self.bind_pat(&arm.pat, slot, fail);
            if !self.dead {
                if let Some(guard) = &arm.guard {
                    let cond = self.emit_expr(guard);
                    if !self.dead {
                        let ok = self.fresh();
                        self.terminate(Term::If { cond, then_seg: ok, else_seg: fail });
                        self.cur = ok;
                        self.dead = false;
                    }
                }
            }
            if !self.dead {
                self.emit_block(&arm.body, Some(dest));
            }
            if !self.dead {
                self.terminate(Term::Jump(join));
            }
            self.cur = fail;
            self.dead = false;
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
            }
            crate::typed::TPat::Ctor { tag, fields } => {
                let ok = self.fresh();
                self.terminate(Term::Tag { slot, tag: *tag, then_seg: ok, else_seg: fail });
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
                            self.insn(Insn::Read { dest, src: slot, offset, cells });
                        }
                        nested => {
                            let dest = self.alloc(field.ty);
                            self.insn(Insn::Read { dest, src: slot, offset, cells });
                            self.bind_pat(nested, dest, fail);
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
                self.terminate(Term::Lit { slot, value: *value, then_seg: ok, else_seg: fail });
                self.cur = ok;
                self.dead = false;
            }
            crate::typed::TPat::Product { fields } => {
                for field in fields {
                    match field.pat.as_ref() {
                        crate::typed::TPat::Wild => {}
                        crate::typed::TPat::Bind(id) => {
                            let dest = self.binding_slot[*id as usize];
                            self.insn(Insn::Take { dest, src: slot, offset: field.offset });
                        }
                        nested => {
                            let dest = self.alloc(field.ty);
                            self.insn(Insn::Take { dest, src: slot, offset: field.offset });
                            self.bind_pat(nested, dest, fail);
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
        if !self.tail_block || self.fn_name == "main" {
            return false;
        }
        match self.result {
            Some(slot) => dest == slot,
            None => ty == Ty::Unit,
        }
    }
}

fn fold_bin(op: BinOp, result: Ty, operand: Ty, a: i64, b: i64) -> Option<i64> {
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
    };
    Some(if op.is_compare() { value } else { result.bits(value) })
}

fn bits(ty: Ty, value: i64) -> u64 {
    match ty {
        Ty::I8 | Ty::U8 => value as u8 as u64,
        Ty::I16 | Ty::U16 | Ty::Usize => value as u16 as u64,
        Ty::I32 | Ty::U32 => value as u32 as u64,
        _ => value as u64,
    }
}

fn less(ty: Ty, a: i64, b: i64) -> bool {
    if ty.is_signed() {
        a < b
    } else {
        bits(ty, a) < bits(ty, b)
    }
}

fn emit(module_name: &str, built: &Built) -> Result<Module, CompileError> {
    let mut module = Module::new(module_name.to_string());
    let mut builder = IrBuilder::new();
    if built.need_alloc || built.need_fail || built.need_heap {
        module.add_function(extern_fn("rfl_fail", IrType::Void, Vec::new()));
    }
    if built.need_alloc || built.need_heap {
        module.add_function(extern_fn("malloc", ptr_ty(), vec![IrType::I32]));
        module.add_function(extern_fn("free", IrType::Void, vec![ptr_ty()]));
    }
    if built.need_io {
        module.add_function(extern_fn("putchar", IrType::Void, vec![IrType::I16]));
    }
    if built.need_alloc {
        module.add_global(GlobalVariable {
            name: "rfl_bump".to_string(),
            var_type: ptr_ty(),
            is_constant: false,
            initializer: Some(null_ptr()),
            linkage: Linkage::Internal,
            symbol_id: None,
        });
    }
    for proc in &built.procs {
        emit_proc(&mut builder, proc, built)?;
        module.add_function(builder.finish_function().unwrap());
    }
    emit_driver(&mut builder, built)?;
    module.add_function(builder.finish_function().unwrap());
    if built.need_alloc {
        emit_frame_alloc(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
        emit_frame_release(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    if built.need_print_num {
        emit_print_u64(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    emit_arc(&mut builder, &mut module, built)?;
    Ok(module)
}

const BUMP_PREV: i64 = 0;
const BUMP_CAP: i64 = 2;
const BUMP_USED: i64 = 3;
const BUMP_DATA: i64 = 4;
const BUMP_CHUNK: i64 = 512;

fn emit_frame_alloc(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_frame_alloc".to_string(), ptr_ty());
    builder.add_parameter(0, IrType::I32);
    let entry = builder.new_label();
    let have = builder.new_label();
    let fit = builder.new_label();
    let grow = builder.new_label();
    let use_words = builder.new_label();
    let use_chunk = builder.new_label();
    let alloc = builder.new_label();
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.create_block(entry).ir()?;
    let words = Value::Temp(builder.build_unary(IrUnaryOp::Trunc, Value::Temp(0), IrType::I16).ir()?);
    let cap_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let chunk = load_bump(builder)?;
    let missing = ptr_is_null(builder, &chunk)?;
    builder.build_branch_cond(missing, grow, have).ir()?;

    builder.create_block(have).ir()?;
    let used = load_cell(builder, &chunk, BUMP_USED, IrType::I16)?;
    let cap = load_cell(builder, &chunk, BUMP_CAP, IrType::I16)?;
    let sum = Value::Temp(builder.build_binary(IrBinaryOp::Add, used.clone(), words.clone(), IrType::I16).ir()?);
    let overflow = Value::Temp(builder.build_binary(IrBinaryOp::Ult, sum.clone(), used.clone(), IrType::I1).ir()?);
    let check = builder.new_label();
    builder.build_branch_cond(overflow, grow, check).ir()?;
    builder.create_block(check).ir()?;
    let too_big = Value::Temp(builder.build_binary(IrBinaryOp::Ugt, sum.clone(), cap, IrType::I1).ir()?);
    builder.build_branch_cond(too_big, grow, fit).ir()?;

    builder.create_block(fit).ir()?;
    let data = gep(builder, &chunk, BUMP_DATA)?;
    let ptr = builder.build_pointer_offset(data, used, ptr_ty()).ir()?;
    store_cell(builder, &chunk, BUMP_USED, sum)?;
    builder.build_return(Some(ptr)).ir()?;

    builder.create_block(grow).ir()?;
    let bigger = Value::Temp(builder.build_binary(IrBinaryOp::Ugt, words.clone(), Value::Constant(BUMP_CHUNK), IrType::I1).ir()?);
    builder.build_branch_cond(bigger, use_words, use_chunk).ir()?;
    builder.create_block(use_words).ir()?;
    builder.build_store(words.clone(), cap_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(alloc).ir()?;
    builder.create_block(use_chunk).ir()?;
    builder.build_store(Value::Constant(BUMP_CHUNK), cap_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(alloc).ir()?;

    builder.create_block(alloc).ir()?;
    let prev = load_bump(builder)?;
    let cap_n = Value::Temp(builder.build_load(cap_slot, IrType::I16).ir()?);
    let total = Value::Temp(builder.build_binary(IrBinaryOp::Add, cap_n.clone(), Value::Constant(BUMP_DATA), IrType::I16).ir()?);
    let size = as_i32(builder, total)?;
    let raw = builder.build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty()).ir()?.unwrap();
    let fresh = fat_temp(raw, BankTag::Mixed);
    let is_null = ptr_is_null(builder, &fresh)?;
    builder.build_branch_cond(is_null, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let prev_slot = gep(builder, &fresh, BUMP_PREV)?;
    builder.build_store(prev, prev_slot, ptr_ty()).ir()?;
    store_cell(builder, &fresh, BUMP_CAP, cap_n)?;
    store_cell(builder, &fresh, BUMP_USED, words)?;
    store_bump(builder, fresh.clone())?;
    let data = gep(builder, &fresh, BUMP_DATA)?;
    builder.build_return(Some(data)).ir()
}

fn emit_frame_release(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_frame_release".to_string(), IrType::Void);
    builder.add_parameter(0, ptr_ty());
    let frame = fat_temp(0, BankTag::Mixed);
    let head = builder.new_label();
    let check = builder.new_label();
    let pop = builder.new_label();
    let rewind = builder.new_label();
    let done = builder.new_label();
    builder.create_block(head).ir()?;
    let chunk = load_bump(builder)?;
    let missing = ptr_is_null(builder, &chunk)?;
    builder.build_branch_cond(missing, done, check).ir()?;

    builder.create_block(check).ir()?;
    let data = gep(builder, &chunk, BUMP_DATA)?;
    let used = load_cell(builder, &chunk, BUMP_USED, IrType::I16)?;
    let frame_bank = bank16_of_ptr(builder, &frame)?;
    let data_bank = bank16_of_ptr(builder, &data)?;
    let same_bank = Value::Temp(builder.build_binary(IrBinaryOp::Eq, frame_bank, data_bank, IrType::I1).ir()?);
    let banks = builder.new_label();
    builder.build_branch_cond(same_bank, banks, pop).ir()?;
    builder.create_block(banks).ir()?;
    let frame_addr = low16_of_ptr(builder, &frame)?;
    let data_addr = low16_of_ptr(builder, &data)?;
    let above = Value::Temp(builder.build_binary(IrBinaryOp::Uge, frame_addr.clone(), data_addr.clone(), IrType::I1).ir()?);
    let span = builder.new_label();
    builder.build_branch_cond(above, span, pop).ir()?;
    builder.create_block(span).ir()?;
    let diff = Value::Temp(builder.build_binary(IrBinaryOp::Sub, frame_addr, data_addr, IrType::I16).ir()?);
    let inside = Value::Temp(builder.build_binary(IrBinaryOp::Ult, diff.clone(), used, IrType::I1).ir()?);
    builder.build_branch_cond(inside, rewind, pop).ir()?;

    builder.create_block(rewind).ir()?;
    store_cell(builder, &chunk, BUMP_USED, diff)?;
    builder.build_branch(done).ir()?;

    builder.create_block(pop).ir()?;
    let prev = load_fat(builder, &chunk, BUMP_PREV)?;
    builder.build_call(Value::Global("free".to_string()), vec![chunk], IrType::Void).ir()?;
    store_bump(builder, prev)?;
    builder.build_branch(head).ir()?;

    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

fn load_bump(builder: &mut IrBuilder) -> Result<Value, CompileError> {
    let temp = builder.build_load(Value::Global("rfl_bump".to_string()), ptr_ty()).ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

fn store_bump(builder: &mut IrBuilder, ptr: Value) -> Result<(), CompileError> {
    builder.build_store(ptr, Value::Global("rfl_bump".to_string()), ptr_ty()).ir()
}

fn ptr_is_null(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder.build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32).ir()?;
    let zero = wide_zero(builder, IrType::I32)?;
    Ok(Value::Temp(builder.build_binary(IrBinaryOp::Eq, Value::Temp(bits), zero, IrType::I1).ir()?))
}

fn low16_of_ptr(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder.build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32).ir()?;
    Ok(Value::Temp(builder.build_unary(IrUnaryOp::Trunc, Value::Temp(bits), IrType::I16).ir()?))
}

fn bank16_of_ptr(builder: &mut IrBuilder, ptr: &Value) -> Result<Value, CompileError> {
    let bits = builder.build_unary(IrUnaryOp::PtrToInt, ptr.clone(), IrType::I32).ir()?;
    let shifted = builder.build_binary(IrBinaryOp::LShr, Value::Temp(bits), Value::Constant(16), IrType::I32).ir()?;
    Ok(Value::Temp(builder.build_unary(IrUnaryOp::Trunc, Value::Temp(shifted), IrType::I16).ir()?))
}

fn as_i32(builder: &mut IrBuilder, value: Value) -> Result<Value, CompileError> {
    Ok(Value::Temp(builder.build_binary(IrBinaryOp::Add, value, Value::Constant(0), IrType::I32).ir()?))
}

fn emit_proc(builder: &mut IrBuilder, proc: &Proc, built: &Built) -> Result<(), CompileError> {
    builder.create_function(fn_symbol(&proc.name), ptr_ty());
    builder.add_parameter(0, ptr_ty());
    let frame = fat_temp(0, BankTag::Mixed);
    // The backend treats block 0 as the function entry and emits no label for
    // it. The entry has to be the first block created in the module so that
    // id is 0, and every later block keeps an id the backend will label.
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    if proc.segs.len() == 1 {
        return emit_seg(builder, proc, built, &frame, &[entry], 0);
    }
    let labels: Vec<u32> = (0..proc.segs.len()).map(|_| builder.new_label()).collect();
    emit_switch(builder, &frame, &labels)?;
    for (index, _) in proc.segs.iter().enumerate() {
        builder.create_block(labels[index]).ir()?;
        emit_seg(builder, proc, built, &frame, &labels, index)?;
    }
    Ok(())
}

fn emit_switch(builder: &mut IrBuilder, frame: &Value, labels: &[u32]) -> Result<(), CompileError> {
    for index in 0..labels.len() {
        let resume = load_cell(builder, frame, RESUME, IrType::I16)?;
        let is_here = builder.build_binary(
            IrBinaryOp::Eq,
            resume,
            Value::Constant(index as i64),
            IrType::I1,
        ).ir()?;
        if index + 1 == labels.len() {
            let bad = builder.new_label();
            builder.build_branch_cond(Value::Temp(is_here), labels[index], bad).ir()?;
            builder.create_block(bad).ir()?;
            builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
            builder.build_return(Some(null_ptr())).ir()?;
            return Ok(());
        }
        let next = builder.new_label();
        builder.build_branch_cond(Value::Temp(is_here), labels[index], next).ir()?;
        builder.create_block(next).ir()?;
    }
    Ok(())
}

fn emit_seg(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    labels: &[u32],
    index: usize,
) -> Result<(), CompileError> {
    if index == 0 {
        zero_managed_slots(builder, proc, frame)?;
        if !proc.captures.is_empty() {
            emit_capture_loads(builder, proc, frame)?;
        }
    }
    for insn in &proc.segs[index].insns {
        emit_insn(builder, proc, built, frame, insn)?;
    }
    match proc.segs[index].term.as_ref().unwrap() {
        Term::Return => emit_return(builder, proc, frame, built.need_alloc),
        Term::Jump(target) => builder.build_branch(labels[*target]).ir(),
        Term::Tag { slot, tag, then_seg, else_seg } => {
            let addr = load_cell(builder, frame, proc.slots[*slot].offset, IrType::I16)?;
            let same = builder.build_binary(IrBinaryOp::Eq, addr, Value::Constant(*tag as i64), IrType::I1).ir()?;
            builder.build_branch_cond(Value::Temp(same), labels[*then_seg], labels[*else_seg]).ir()
        }
        Term::If { cond, then_seg, else_seg } => {
            let cond = load_slot(builder, proc, frame, *cond)?;
            builder.build_branch_cond(cond, labels[*then_seg], labels[*else_seg]).ir()
        }
        Term::Call { callee, args, dest, next } => {
            emit_call(builder, proc, built, frame, callee, args, *dest, *next)
        }
        Term::Apply { slot, args, arg_tys, dest, next } => {
            emit_apply(builder, proc, frame, *slot, args, arg_tys, *dest, *next)
        }
        Term::Fail => {
            builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
            builder.build_return(Some(null_ptr())).ir()
        }
        Term::Lit { slot, value, then_seg, else_seg } => {
            let ty = proc.slots[*slot].ty;
            let got = load_slot(builder, proc, frame, *slot)?;
            let same = builder.build_binary(IrBinaryOp::Eq, got, Value::Constant(ty.bits(*value)), IrType::I1).ir()?;
            builder.build_branch_cond(Value::Temp(same), labels[*then_seg], labels[*else_seg]).ir()
        }
        Term::Tail { callee, args } => emit_tail_call(builder, proc, built, frame, callee, args),
    }
}

fn emit_insn(builder: &mut IrBuilder, proc: &Proc, built: &Built, frame: &Value, insn: &Insn) -> Result<(), CompileError> {
    match insn {
        Insn::Const { slot, value } => {
            let slot = &proc.slots[*slot];
            let ptr = gep(builder, frame, slot.offset)?;
            builder.build_store(Value::Constant(*value), ptr, ir_ty(slot.ty)).ir()
        }
        Insn::Copy { dest, src } => {
            if *dest != *src && keeps(proc, proc.slots[*dest].ty) {
                release_managed(builder, proc, frame, *dest)?;
            }
            if *dest != *src {
                copy_slot(builder, proc, frame, *src, proc, frame, *dest)
            } else {
                Ok(())
            }
        }
        Insn::Bin { dest, op, lhs, rhs } => emit_bin(builder, proc, frame, *dest, *op, *lhs, *rhs),
        Insn::Neg { dest, src } => {
            let slot = &proc.slots[*src];
            let value = load_slot(builder, proc, frame, *src)?;
            let negated = if matches!(slot.ty, Ty::I8) {
                let wide = builder.build_unary(IrUnaryOp::ZExt, value, IrType::I16).ir()?;
                let zero = Value::Constant(0);
                let sub = builder.build_binary(IrBinaryOp::Sub, zero, Value::Temp(wide), IrType::I16).ir()?;
                Value::Temp(builder.build_unary(IrUnaryOp::Trunc, Value::Temp(sub), IrType::I8).ir()?)
            } else {
                let ty = ir_ty(slot.ty);
                let zero = wide_zero(builder, ty.clone())?;
                Value::Temp(builder.build_binary(IrBinaryOp::Sub, zero, value, ty).ir()?)
            };
            store_slot(builder, proc, frame, *dest, negated)
        }
        Insn::Not { dest, src } => {
            let value = load_slot(builder, proc, frame, *src)?;
            let flipped = builder.build_binary(IrBinaryOp::Eq, value, Value::Constant(0), IrType::I1).ir()?;
            store_slot(builder, proc, frame, *dest, Value::Temp(flipped))
        }
        Insn::Make { dest, tag, fields } => emit_make(builder, proc, frame, *dest, *tag, fields),
        Insn::Read { dest, src, offset, cells } => emit_read(builder, proc, frame, *dest, *src, *offset, *cells),
        Insn::Print { slot, ty, newline } => emit_print(builder, proc, frame, *slot, *ty, *newline),
        Insn::PrintLit { chars, newline } => {
            emit_chars(builder, chars)?;
            if *newline {
                putchar(builder, Value::Constant(10))?;
            }
            Ok(())
        }
        Insn::Closure { dest, func } => {
            let (func_id, frame_cells, captures) = {
                let callee = built.procs.iter().find(|item| item.name == *func).unwrap();
                (callee.func_id, callee.frame_cells, callee.captures.clone())
            };
            let (addr, bank) = if captures.is_empty() {
                (Value::Constant(0), Value::Constant(0))
            } else {
                emit_env(builder, proc, frame, &captures)?
            };
            release_managed(builder, proc, frame, *dest)?;
            let base = proc.slots[*dest].offset;
            store_cell(builder, frame, base, Value::Constant(func_id as i64))?;
            store_cell(builder, frame, base + 1, Value::Constant(frame_cells))?;
            store_cell(builder, frame, base + 2, addr)?;
            store_cell(builder, frame, base + 3, bank)
        }
        Insn::VecLit { dest, elems, elem } => emit_vec_lit(builder, proc, frame, *dest, elems, *elem),
        Insn::ArrayLit { dest, elems, elem } => emit_array_lit(builder, proc, frame, *dest, elems, *elem),
        Insn::SliceNew { dest, base, start, end, inclusive, elem } => {
            emit_slice_new(builder, proc, frame, *dest, *base, *start, *end, *inclusive, *elem)
        }
        Insn::ToVec { dest, src, elem } => emit_to_vec(builder, proc, frame, *dest, *src, *elem),
        Insn::Index { dest, vec, index, elem } => emit_vec_index(builder, proc, frame, *dest, *vec, *index, *elem),
        Insn::Len { dest, vec } => emit_vec_len(builder, proc, frame, *dest, *vec),
        Insn::Empty { dest, vec } => emit_vec_empty(builder, proc, frame, *dest, *vec),
        Insn::Clear { vec } => emit_vec_clear(builder, proc, frame, *vec),
        Insn::Pop { vec, value, dest, elem, some_tag, none_tag } => {
            emit_vec_pop(builder, proc, frame, *vec, *value, *dest, *elem, *some_tag, *none_tag)
        }
        Insn::Get { vec, index, value, dest, elem, some_tag, none_tag } => {
            emit_vec_get(builder, proc, frame, *vec, *index, *value, *dest, *elem, *some_tag, *none_tag)
        }
        Insn::Borrow { dest, slot } => emit_borrow(builder, proc, frame, *dest, *slot),
        Insn::Try { src, dest, result, ok_tag, fail_tag, payload, fail_slot, fail } => {
            emit_try(builder, proc, frame, *src, *dest, *result, *ok_tag, *fail_tag, *payload, *fail_slot, *fail, built.need_alloc)
        }
        Insn::Push { vec, value, elem } => emit_vec_push(builder, proc, frame, *vec, *value, *elem),
        Insn::IndexStore { vec, index, value, elem } => {
            emit_vec_index_store(builder, proc, frame, *vec, *index, *value, *elem)
        }
        Insn::VecStore { dest, src, transfer } => emit_vec_store(builder, proc, frame, *dest, *src, *transfer),
        Insn::Putc { slot } => {
            let ty = proc.slots[*slot].ty;
            let value = load_slot(builder, proc, frame, *slot)?;
            let cell = if width(ty) > 16 {
                Value::Temp(builder.build_unary(IrUnaryOp::Trunc, value, IrType::I16).ir()?)
            } else if matches!(ty, Ty::I8 | Ty::U8) {
                Value::Temp(builder.build_unary(IrUnaryOp::ZExt, value, IrType::I16).ir()?)
            } else {
                value
            };
            putchar(builder, cell)
        }
        Insn::Record { dest, fields } => emit_record(builder, proc, frame, *dest, fields),
        Insn::Take { dest, src, offset } => emit_take(builder, proc, frame, *dest, *src, *offset),
        Insn::FieldStore { dest, offset, src } => emit_field_store(builder, proc, frame, *dest, *offset, *src),
        Insn::Cast { dest, src, from, to } => {
            let value = load_slot(builder, proc, frame, *src)?;
            let cast = cast_value(builder, value, *from, *to)?;
            store_slot(builder, proc, frame, *dest, cast)
        }
    }
}

fn emit_bin(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, op: BinOp, lhs: usize, rhs: usize) -> Result<(), CompileError> {
    let ty = proc.slots[lhs].ty;
    let left = load_slot(builder, proc, frame, lhs)?;
    let right = load_slot(builder, proc, frame, rhs)?;
    let left = widen_for_op(builder, left, ty)?;
    let right = widen_for_op(builder, right, ty)?;
    let ir = ir_op(op, ty);
    let result_ty = if op.is_compare() { IrType::I1 } else { arith_ty(ty) };
    if matches!(op, BinOp::Div | BinOp::Rem) {
        let ok = builder.new_label();
        let bad = builder.new_label();
        let zero = wide_zero(builder, arith_ty(ty))?;
        let is_zero = builder.build_binary(IrBinaryOp::Eq, right.clone(), zero, IrType::I1).ir()?;
        builder.build_branch_cond(Value::Temp(is_zero), bad, ok).ir()?;
        builder.create_block(bad).ir()?;
        builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
        builder.build_return(Some(null_ptr())).ir()?;
        builder.create_block(ok).ir()?;
    }
    let value = builder.build_binary(ir, left, right, result_ty).ir()?;
    let value = if matches!(ty, Ty::I8 | Ty::U8) && op.is_arith() {
        Value::Temp(builder.build_unary(IrUnaryOp::Trunc, Value::Temp(value), IrType::I8).ir()?)
    } else {
        Value::Temp(value)
    };
    store_slot(builder, proc, frame, dest, value)
}

fn emit_return(builder: &mut IrBuilder, proc: &Proc, frame: &Value, release: bool) -> Result<(), CompileError> {
    let addr = load_cell(builder, frame, CALLER, IrType::I16)?;
    let is_root = builder.build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1).ir()?;
    let root = builder.new_label();
    let caller = builder.new_label();
    builder.build_branch_cond(Value::Temp(is_root), root, caller).ir()?;
    builder.create_block(root).ir()?;
    release_managed_slots(builder, proc, frame)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(caller).ir()?;
    let caller_ptr = load_fat(builder, frame, CALLER)?;
    if let Some(result) = proc.result_slot {
        let dest = load_cell(builder, frame, DEST, IrType::I16)?;
        let slot = builder.build_pointer_offset(caller_ptr.clone(), dest, ptr_ty()).ir()?;
        if cell_wise(proc.ret) {
            let start = proc.slots[result].offset;
            let mut words = Vec::new();
            for index in 0..proc.ret.cells() {
                words.push(load_cell(builder, frame, start + index, IrType::I16)?);
            }
            if keeps(proc, proc.ret) {
                release_managed_through(builder, proc, &slot, proc.ret)?;
            }
            for (index, value) in words.into_iter().enumerate() {
                let cell = builder.build_pointer_offset(slot.clone(), Value::Constant(index as i64), ptr_ty()).ir()?;
                builder.build_store(value, cell, IrType::I16).ir()?;
            }
            if keeps(proc, proc.ret) {
                retain_managed_through(builder, proc, &slot, proc.ret)?;
            }
        } else {
            let value = load_slot(builder, proc, frame, result)?;
            builder.build_store(value, slot, ir_ty(proc.ret)).ir()?;
        }
    }
    release_managed_slots(builder, proc, frame)?;
    if release {
        let saved = builder.build_alloca(ptr_ty(), None).ir()?;
        builder.build_store(caller_ptr, saved.clone(), ptr_ty()).ir()?;
        builder.build_call(Value::Global("rfl_frame_release".to_string()), vec![frame.clone()], IrType::Void).ir()?;
        let caller_ptr = load_ptr_slot(builder, &saved)?;
        builder.build_return(Some(caller_ptr)).ir()
    } else {
        builder.build_return(Some(caller_ptr)).ir()
    }
}

fn keeps(proc: &Proc, ty: Ty) -> bool {
    match ty {
        Ty::Struct { id, .. } => proc.structs[id as usize].iter().any(|field| keeps(proc, *field)),
        Ty::Tuple { id, .. } => proc.tuples[id as usize].iter().any(|field| keeps(proc, *field)),
        other => other.is_managed(),
    }
}

fn retain_tree(builder: &mut IrBuilder, proc: &Proc, frame: &Value, offset: i64, ty: Ty) -> Result<(), CompileError> {
    match ty {
        Ty::Struct { id, .. } => {
            let mut at = offset;
            for field in proc.structs[id as usize].clone() {
                retain_tree(builder, proc, frame, at, field)?;
                at += field.cells();
            }
            Ok(())
        }
        Ty::Tuple { id, .. } => {
            let mut at = offset;
            for field in proc.tuples[id as usize].clone() {
                retain_tree(builder, proc, frame, at, field)?;
                at += field.cells();
            }
            Ok(())
        }
        other => retain_value_at(builder, frame, offset, other),
    }
}

fn release_tree(builder: &mut IrBuilder, proc: &Proc, frame: &Value, offset: i64, ty: Ty) -> Result<(), CompileError> {
    match ty {
        Ty::Struct { id, .. } => {
            let mut at = offset;
            for field in proc.structs[id as usize].clone() {
                release_tree(builder, proc, frame, at, field)?;
                at += field.cells();
            }
            Ok(())
        }
        Ty::Tuple { id, .. } => {
            let mut at = offset;
            for field in proc.tuples[id as usize].clone() {
                release_tree(builder, proc, frame, at, field)?;
                at += field.cells();
            }
            Ok(())
        }
        Ty::Vec(_) | Ty::Slice(_) => release_vec(builder, frame, offset),
        Ty::Adt(id) => {
            let tag = load_cell(builder, frame, offset, IrType::I16)?;
            let addr = load_cell(builder, frame, offset + 1, IrType::I16)?;
            let bank = load_cell(builder, frame, offset + 2, IrType::I16)?;
            call_if_live(builder, &format!("rfl_drop_adt_{id}"), vec![tag, addr, bank])?;
            store_cell(builder, frame, offset + 1, Value::Constant(0))?;
            store_cell(builder, frame, offset + 2, Value::Constant(0))
        }
        Ty::Func(_) => {
            let id = load_cell(builder, frame, offset, IrType::I16)?;
            let addr = load_cell(builder, frame, offset + 2, IrType::I16)?;
            let bank = load_cell(builder, frame, offset + 3, IrType::I16)?;
            call_if_live(builder, "rfl_drop_func", vec![id, addr, bank])?;
            store_cell(builder, frame, offset + 2, Value::Constant(0))?;
            store_cell(builder, frame, offset + 3, Value::Constant(0))
        }
        _ => Ok(()),
    }
}

fn emit_record(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, fields: &[usize]) -> Result<(), CompileError> {
    if keeps(proc, proc.slots[dest].ty) {
        release_tree(builder, proc, frame, proc.slots[dest].offset, proc.slots[dest].ty)?;
    }
    let mut offset = proc.slots[dest].offset;
    for field in fields {
        let ty = proc.slots[*field].ty;
        for index in 0..ty.cells() {
            let value = load_cell(builder, frame, proc.slots[*field].offset + index, IrType::I16)?;
            store_cell(builder, frame, offset, value)?;
            offset += 1;
        }
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, offset - ty.cells(), ty)?;
        }
    }
    Ok(())
}

fn emit_take(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, src: usize, offset: i64) -> Result<(), CompileError> {
    let ty = proc.slots[dest].ty;
    let from = proc.slots[src].offset + offset;
    if keeps(proc, ty) {
        release_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
    }
    for index in 0..ty.cells() {
        let value = load_cell(builder, frame, from + index, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + index, value)?;
    }
    if keeps(proc, ty) {
        retain_tree(builder, proc, frame, proc.slots[dest].offset, ty)?;
    }
    Ok(())
}

fn emit_field_store(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, offset: i64, src: usize) -> Result<(), CompileError> {
    let ty = proc.slots[src].ty;
    let at = proc.slots[dest].offset + offset;
    if keeps(proc, ty) {
        release_tree(builder, proc, frame, at, ty)?;
    }
    for index in 0..ty.cells() {
        let value = load_cell(builder, frame, proc.slots[src].offset + index, IrType::I16)?;
        store_cell(builder, frame, at + index, value)?;
    }
    if keeps(proc, ty) {
        retain_tree(builder, proc, frame, at, ty)?;
    }
    Ok(())
}

fn emit_tail_call(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    callee: &str,
    args: &[usize],
) -> Result<(), CompileError> {
    let callee_proc = built.procs.iter().find(|proc| proc.name == callee).unwrap();
    let mut saved = Vec::new();
    for arg in args {
        let ty = proc.slots[*arg].ty;
        if keeps(proc, ty) {
            retain_tree(builder, proc, frame, proc.slots[*arg].offset, ty)?;
        }
        let mut words = Vec::new();
        if ty != Ty::Unit {
            if cell_wise(ty) {
                for index in 0..ty.cells() {
                    let value = load_cell(builder, frame, proc.slots[*arg].offset + index, IrType::I16)?;
                    let mem = builder.build_alloca(IrType::I16, None).ir()?;
                    builder.build_store(value, mem.clone(), IrType::I16).ir()?;
                    words.push(mem);
                }
            } else {
                let value = load_slot(builder, proc, frame, *arg)?;
                let mem = builder.build_alloca(ir_ty(ty), None).ir()?;
                builder.build_store(value, mem.clone(), ir_ty(ty)).ir()?;
                words.push(mem);
            }
        }
        saved.push(words);
    }
    let caller_mem = builder.build_alloca(ptr_ty(), None).ir()?;
    let caller_ptr = load_fat(builder, frame, CALLER)?;
    builder.build_store(caller_ptr, caller_mem.clone(), ptr_ty()).ir()?;
    let dest_mem = builder.build_alloca(IrType::I16, None).ir()?;
    let dest_off = load_cell(builder, frame, DEST, IrType::I16)?;
    builder.build_store(dest_off, dest_mem.clone(), IrType::I16).ir()?;
    release_managed_slots(builder, proc, frame)?;
    builder.build_call(Value::Global("rfl_frame_release".to_string()), vec![frame.clone()], IrType::Void).ir()?;
    let words = i32_const(builder, callee_proc.frame_cells)?;
    let raw = builder.build_call(Value::Global("rfl_frame_alloc".to_string()), vec![words], ptr_ty()).ir()?.unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &callee_frame, FUNC_ID, Value::Constant(callee_proc.func_id as i64))?;
    let saved_caller = load_ptr_slot(builder, &caller_mem)?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder.build_store(saved_caller, caller_slot, ptr_ty()).ir()?;
    let saved_dest = Value::Temp(builder.build_load(dest_mem, IrType::I16).ir()?);
    store_cell(builder, &callee_frame, DEST, saved_dest)?;
    for (words, param) in saved.iter().zip(&callee_proc.param_slots) {
        let ty = callee_proc.slots[*param].ty;
        let base = callee_proc.slots[*param].offset;
        if cell_wise(ty) {
            for (index, mem) in words.iter().enumerate() {
                let value = Value::Temp(builder.build_load(mem.clone(), IrType::I16).ir()?);
                store_cell(builder, &callee_frame, base + index as i64, value)?;
            }
        } else if let Some(mem) = words.first() {
            let value = Value::Temp(builder.build_load(mem.clone(), ir_ty(ty)).ir()?);
            let ptr = gep(builder, &callee_frame, base)?;
            builder.build_store(value, ptr, ir_ty(ty)).ir()?;
        }
    }
    builder.build_return(Some(callee_frame)).ir()
}

fn emit_call(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    callee: &str,
    args: &[usize],
    dest: usize,
    next: usize,
) -> Result<(), CompileError> {
    let callee_proc = built.procs.iter().find(|proc| proc.name == callee).unwrap();
    let words = i32_const(builder, callee_proc.frame_cells)?;
    let raw = builder.build_call(
        Value::Global("rfl_frame_alloc".to_string()),
        vec![words],
        ptr_ty(),
    ).ir()?.unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &callee_frame, FUNC_ID, Value::Constant(callee_proc.func_id as i64))?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder.build_store(frame.clone(), caller_slot, ptr_ty()).ir()?;
    store_cell(builder, &callee_frame, DEST, Value::Constant(proc.slots[dest].offset))?;
    for (arg, param) in args.iter().zip(&callee_proc.param_slots) {
        copy_slot(builder, proc, frame, *arg, callee_proc, &callee_frame, *param)?;
    }
    store_cell(builder, frame, RESUME, Value::Constant(next as i64))?;
    builder.build_return(Some(callee_frame)).ir()
}

fn emit_apply(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    args: &[usize],
    arg_tys: &[Ty],
    dest: usize,
    next: usize,
) -> Result<(), CompileError> {
    let base = proc.slots[slot].offset;
    let id = load_cell(builder, frame, base, IrType::I16)?;
    let words = load_cell(builder, frame, base + 1, IrType::I16)?;
    let size = as_i32(builder, words)?;
    let raw = builder.build_call(Value::Global("rfl_frame_alloc".to_string()), vec![size], ptr_ty()).ir()?.unwrap();
    let callee_frame = fat_temp(raw, BankTag::Mixed);
    store_cell(builder, &callee_frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &callee_frame, FUNC_ID, id)?;
    let caller_slot = gep(builder, &callee_frame, CALLER)?;
    builder.build_store(frame.clone(), caller_slot, ptr_ty()).ir()?;
    store_cell(builder, &callee_frame, DEST, Value::Constant(proc.slots[dest].offset))?;
    let mut offset = PAYLOAD + proc.slots[dest].ty.cells();
    for (arg, ty) in args.iter().zip(arg_tys) {
        copy_to_offset(builder, proc, frame, *arg, &callee_frame, offset)?;
        offset += ty.cells();
    }
    let env_addr = load_cell(builder, frame, base + 2, IrType::I16)?;
    let env_bank = load_cell(builder, frame, base + 3, IrType::I16)?;
    store_cell(builder, &callee_frame, offset, env_addr)?;
    store_cell(builder, &callee_frame, offset + 1, env_bank)?;
    store_cell(builder, frame, RESUME, Value::Constant(next as i64))?;
    builder.build_return(Some(callee_frame)).ir()
}

fn copy_to_offset(
    builder: &mut IrBuilder,
    src_proc: &Proc,
    src_frame: &Value,
    src: usize,
    dest_frame: &Value,
    dest_offset: i64,
) -> Result<(), CompileError> {
    let ty = src_proc.slots[src].ty;
    if ty == Ty::Unit {
        return Ok(());
    }
    if cell_wise(ty) {
        for index in 0..ty.cells() {
            let value = load_cell(builder, src_frame, src_proc.slots[src].offset + index, IrType::I16)?;
            store_cell(builder, dest_frame, dest_offset + index, value)?;
        }
        if keeps(src_proc, ty) {
            retain_tree(builder, src_proc, dest_frame, dest_offset, ty)?;
        }
        return Ok(());
    }
    let value = load_slot(builder, src_proc, src_frame, src)?;
    let ptr = gep(builder, dest_frame, dest_offset)?;
    builder.build_store(value, ptr, ir_ty(ty)).ir()
}

fn emit_driver(builder: &mut IrBuilder, built: &Built) -> Result<(), CompileError> {
    let main = built.procs.iter().find(|proc| proc.name == "main").unwrap();
    builder.create_function("main".to_string(), IrType::I16);
    let entry = builder.new_label();
    let head = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let frame = builder.build_alloca(IrType::I16, Some(Value::Constant(main.frame_cells))).ir()?;
    store_cell(builder, &frame, RESUME, Value::Constant(0))?;
    store_cell(builder, &frame, FUNC_ID, Value::Constant(main.func_id as i64))?;
    let caller_slot = gep(builder, &frame, CALLER)?;
    builder.build_store(null_ptr(), caller_slot, ptr_ty()).ir()?;
    store_cell(builder, &frame, DEST, Value::Constant(0))?;
    let cur_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_store(frame.clone(), cur_slot.clone(), ptr_ty()).ir()?;
    let next_slot = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_branch(head).ir()?;

    builder.create_block(head).ir()?;
    let cur = load_ptr_slot(builder, &cur_slot)?;
    let id = load_cell(builder, &cur, FUNC_ID, IrType::I16)?;
    let merge = builder.new_label();
    emit_dispatch(builder, built, &cur, &next_slot, id, merge)?;
    builder.create_block(merge).ir()?;
    let next = load_ptr_slot(builder, &next_slot)?;
    let bits = builder.build_unary(IrUnaryOp::PtrToInt, next.clone(), IrType::I32).ir()?;
    let is_null = builder.build_binary(IrBinaryOp::Eq, Value::Temp(bits), Value::Constant(0), IrType::I1).ir()?;
    let again = builder.new_label();
    builder.build_branch_cond(Value::Temp(is_null), done, again).ir()?;
    builder.create_block(again).ir()?;
    builder.build_store(next, cur_slot, ptr_ty()).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    if main.ret == Ty::Unit {
        builder.build_return(Some(Value::Constant(0))).ir()
    } else {
        let value = load_cell(builder, &frame, PAYLOAD, IrType::I16)?;
        builder.build_return(Some(value)).ir()
    }
}

fn emit_dispatch(
    builder: &mut IrBuilder,
    built: &Built,
    cur: &Value,
    next_slot: &Value,
    id: Value,
    merge: u32,
) -> Result<(), CompileError> {
    for (index, proc) in built.procs.iter().enumerate() {
        let call = builder.new_label();
        let miss = builder.new_label();
        let is_here = builder.build_binary(
            IrBinaryOp::Eq,
            id.clone(),
            Value::Constant(index as i64),
            IrType::I1,
        ).ir()?;
        builder.build_branch_cond(Value::Temp(is_here), call, miss).ir()?;
        builder.create_block(call).ir()?;
        let raw = builder.build_call(
            Value::Global(fn_symbol(&proc.name)),
            vec![cur.clone()],
            ptr_ty(),
        ).ir()?.unwrap();
        builder.build_store(fat_temp(raw, BankTag::Mixed), next_slot.clone(), ptr_ty()).ir()?;
        builder.build_branch(merge).ir()?;
        builder.create_block(miss).ir()?;
        if index + 1 == built.procs.len() {
            if built.need_fail {
                builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
            }
            builder.build_return(Some(Value::Constant(1))).ir()?;
        }
    }
    Ok(())
}

fn emit_capture_loads(builder: &mut IrBuilder, proc: &Proc, frame: &Value) -> Result<(), CompileError> {
    let env_at = proc.env_at.expect("a capturing function has an environment slot");
    let addr = load_cell(builder, frame, env_at, IrType::I16)?;
    let bank = load_cell(builder, frame, env_at + 1, IrType::I16)?;
    let env = fat_from_parts(builder, addr, bank)?;
    for cap in &proc.captures {
        let dest = proc.slots[cap.slot].offset;
        for index in 0..cap.cells {
            let value = load_cell(builder, &env, cap.offset + index, IrType::I16)?;
            store_cell(builder, frame, dest + index, value)?;
        }
        if keeps(proc, proc.slots[cap.slot].ty) {
            retain_managed(builder, proc, frame, cap.slot)?;
        }
    }
    Ok(())
}

fn emit_env(builder: &mut IrBuilder, proc: &Proc, frame: &Value, captures: &[LowerCap]) -> Result<(Value, Value), CompileError> {
    let words = 1 + captures.iter().map(|cap| cap.cells).sum::<i64>();
    let size = i32_const(builder, words)?;
    let raw = builder.build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty()).ir()?.unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Constant(1))?;
    for cap in captures {
        let parent = proc.binding_slots[cap.parent as usize];
        let src = proc.slots[parent].offset;
        for index in 0..cap.cells {
            let value = load_cell(builder, frame, src + index, IrType::I16)?;
            store_object_cell(builder, &obj, cap.offset + index, value)?;
        }
        if keeps(proc, proc.slots[parent].ty) {
            retain_managed(builder, proc, frame, parent)?;
        }
    }
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    Ok((addr, bank))
}

const VEC_REFS: i64 = 0;
const VEC_LEN: i64 = 1;
const VEC_CAP: i64 = 2;
const VEC_DATA: i64 = 3;
/// Refcount, then tag, then constructor fields.
const OBJ_REFS: i64 = 0;
const OBJ_TAG: i64 = 1;
const OBJ_FIELDS: i64 = 2;

fn zero_managed_slots(builder: &mut IrBuilder, proc: &Proc, frame: &Value) -> Result<(), CompileError> {
    for (index, slot) in proc.slots.iter().enumerate() {
        if keeps(proc, slot.ty) && !proc.param_slots.contains(&index) {
            for cell in 0..slot.ty.cells() {
                store_cell(builder, frame, slot.offset + cell, Value::Constant(0))?;
            }
        }
    }
    Ok(())
}

fn release_managed_slots(builder: &mut IrBuilder, proc: &Proc, frame: &Value) -> Result<(), CompileError> {
    for index in 0..proc.slots.len() {
        if keeps(proc, proc.slots[index].ty) {
            release_managed(builder, proc, frame, index)?;
        }
    }
    Ok(())
}

fn retain_vec(builder: &mut IrBuilder, frame: &Value, offset: i64) -> Result<(), CompileError> {
    let addr = load_cell(builder, frame, offset, IrType::I16)?;
    let bank = load_cell(builder, frame, offset + 1, IrType::I16)?;
    adjust_refcount(builder, addr, bank, true)
}

fn release_vec(builder: &mut IrBuilder, frame: &Value, offset: i64) -> Result<(), CompileError> {
    let addr = load_cell(builder, frame, offset, IrType::I16)?;
    let bank = load_cell(builder, frame, offset + 1, IrType::I16)?;
    adjust_refcount(builder, addr, bank, false)
}

/// `retain` increments. Otherwise the last reference frees the buffer and any earlier one decrements.
fn adjust_refcount(builder: &mut IrBuilder, addr: Value, bank: Value, retain: bool) -> Result<(), CompileError> {
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), skip, go).ir()?;
    builder.create_block(go).ir()?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    if retain {
        let next = builder.build_binary(IrBinaryOp::Add, refs, Value::Constant(1), IrType::I16).ir()?;
        store_object_cell(builder, &obj, VEC_REFS, Value::Temp(next))?;
        builder.build_branch(skip).ir()?;
    } else {
        let last = builder.build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1).ir()?;
        let free_it = builder.new_label();
        let dec = builder.new_label();
        builder.build_branch_cond(Value::Temp(last), free_it, dec).ir()?;
        builder.create_block(dec).ir()?;
        let next = builder.build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16).ir()?;
        store_object_cell(builder, &obj, VEC_REFS, Value::Temp(next))?;
        builder.build_branch(skip).ir()?;
        builder.create_block(free_it).ir()?;
        builder.build_call(Value::Global("free".to_string()), vec![obj], IrType::Void).ir()?;
        builder.build_branch(skip).ir()?;
    }
    builder.create_block(skip).ir()?;
    Ok(())
}

fn emit_vec_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    transfer: bool,
) -> Result<(), CompileError> {
    release_vec(builder, frame, proc.slots[dest].offset)?;
    let cells = proc.slots[src].ty.cells();
    for index in 0..cells {
        let value = load_cell(builder, frame, proc.slots[src].offset + index, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + index, value)?;
    }
    if transfer {
        store_cell(builder, frame, proc.slots[src].offset, Value::Constant(0))?;
        store_cell(builder, frame, proc.slots[src].offset + 1, Value::Constant(0))
    } else {
        retain_vec(builder, frame, proc.slots[dest].offset)
    }
}

fn runtime_fail(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()
}

fn exclusive_end(builder: &mut IrBuilder, end: Value, inclusive: bool) -> Result<Value, CompileError> {
    if !inclusive {
        return Ok(end);
    }
    let exclusive = Value::Temp(builder.build_binary(IrBinaryOp::Add, end.clone(), Value::Constant(1), IrType::I16).ir()?);
    let wrapped = builder.build_binary(IrBinaryOp::Ule, exclusive.clone(), end, IrType::I1).ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(wrapped), bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder)?;
    builder.create_block(ok).ir()?;
    Ok(exclusive)
}

fn check_range(builder: &mut IrBuilder, start: Value, exclusive: Value, len: Value) -> Result<(), CompileError> {
    let bad = builder.new_label();
    let mid = builder.new_label();
    let ok = builder.new_label();
    let start_ok = builder.build_binary(IrBinaryOp::Ule, start, exclusive.clone(), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(start_ok), mid, bad).ir()?;
    builder.create_block(mid).ir()?;
    let end_ok = builder.build_binary(IrBinaryOp::Ule, exclusive, len, IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(end_ok), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder)?;
    builder.create_block(ok).ir()?;
    Ok(())
}

fn store_empty_slice(builder: &mut IrBuilder, frame: &Value, offset: i64) -> Result<(), CompileError> {
    for cell in 0..4 {
        store_cell(builder, frame, offset + cell, Value::Constant(0))?;
    }
    Ok(())
}

fn emit_array_lit(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    elems: &[usize],
    elem: Ty,
) -> Result<(), CompileError> {
    let mut at = proc.slots[dest].offset;
    for slot in elems {
        for cell in 0..elem.cells() {
            let value = load_cell(builder, frame, proc.slots[*slot].offset + cell, IrType::I16)?;
            store_cell(builder, frame, at, value)?;
            at += 1;
        }
    }
    Ok(())
}

fn copy_inline_elem(
    builder: &mut IrBuilder,
    frame: &Value,
    base: i64,
    index: Value,
    width: i64,
    dest: i64,
) -> Result<(), CompileError> {
    let scaled = scale_index(builder, index, width)?;
    let origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, Value::Constant(base), scaled, IrType::I16).ir()?);
    for cell in 0..width {
        let off = Value::Temp(builder.build_binary(IrBinaryOp::Add, origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let ptr = builder.build_pointer_offset(frame.clone(), off, ptr_ty()).ir()?;
        let word = Value::Temp(builder.build_load(ptr, IrType::I16).ir()?);
        store_cell(builder, frame, dest + cell, word)?;
    }
    Ok(())
}

fn store_inline_elem(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    value: usize,
    base: i64,
    index: Value,
    width: i64,
) -> Result<(), CompileError> {
    let scaled = scale_index(builder, index, width)?;
    let origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, Value::Constant(base), scaled, IrType::I16).ir()?);
    for cell in 0..width {
        let word = load_cell(builder, frame, proc.slots[value].offset + cell, IrType::I16)?;
        let off = Value::Temp(builder.build_binary(IrBinaryOp::Add, origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let ptr = builder.build_pointer_offset(frame.clone(), off, ptr_ty()).ir()?;
        builder.build_store(word, ptr, IrType::I16).ir()?;
    }
    Ok(())
}

fn emit_array_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    array: usize,
    index: usize,
) -> Result<(), CompileError> {
    let Ty::Array { len, width, .. } = proc.slots[array].ty else {
        return Ok(());
    };
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), Value::Constant(len as i64), IrType::I1).ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder)?;
    builder.create_block(ok).ir()?;
    copy_inline_elem(builder, frame, proc.slots[array].offset, at, width as i64, proc.slots[dest].offset)
}

fn emit_array_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    array: usize,
    index: usize,
    value: usize,
) -> Result<(), CompileError> {
    let Ty::Array { len, width, .. } = proc.slots[array].ty else {
        return Ok(());
    };
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), Value::Constant(len as i64), IrType::I1).ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder)?;
    builder.create_block(ok).ir()?;
    store_inline_elem(builder, proc, frame, value, proc.slots[array].offset, at, width as i64)
}

fn emit_array_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    array: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let Ty::Array { len, width, .. } = proc.slots[array].ty else {
        return Ok(());
    };
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), Value::Constant(len as i64), IrType::I1).ir()?;
    let none = builder.new_label();
    let ok = builder.new_label();
    let done = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, none).ir()?;
    builder.create_block(ok).ir()?;
    copy_inline_elem(builder, frame, proc.slots[array].offset, at, width as i64, proc.slots[value].offset)?;
    emit_make(builder, proc, frame, dest, some_tag, &[(value, elem.cells())])?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_slice_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    slice: usize,
    index: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    let base = proc.slots[slice].offset;
    let len = load_cell(builder, frame, base + 3, IrType::I16)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1).ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    runtime_fail(builder)?;
    builder.create_block(ok).ir()?;
    let start = load_cell(builder, frame, base + 2, IrType::I16)?;
    let elem_at = Value::Temp(builder.build_binary(IrBinaryOp::Add, start, at, IrType::I16).ir()?);
    let addr = load_cell(builder, frame, base, IrType::I16)?;
    let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let scaled = scale_index(builder, elem_at, elem.cells())?;
    let data = Value::Temp(builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?);
    copy_object_cells(builder, proc, frame, &obj, data, dest, elem.cells())
}

fn emit_slice_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slice: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let base = proc.slots[slice].offset;
    let len = load_cell(builder, frame, base + 3, IrType::I16)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1).ir()?;
    let none = builder.new_label();
    let ok = builder.new_label();
    let done = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, none).ir()?;
    builder.create_block(ok).ir()?;
    let start = load_cell(builder, frame, base + 2, IrType::I16)?;
    let elem_at = Value::Temp(builder.build_binary(IrBinaryOp::Add, start, at, IrType::I16).ir()?);
    let addr = load_cell(builder, frame, base, IrType::I16)?;
    let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let scaled = scale_index(builder, elem_at, elem.cells())?;
    let data = Value::Temp(builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?);
    copy_object_cells(builder, proc, frame, &obj, data, value, elem.cells())?;
    emit_make(builder, proc, frame, dest, some_tag, &[(value, elem.cells())])?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_slice_new(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    base: usize,
    start: usize,
    end: usize,
    inclusive: bool,
    elem: Ty,
) -> Result<(), CompileError> {
    let start = load_slot(builder, proc, frame, start)?;
    let end = load_slot(builder, proc, frame, end)?;
    let exclusive = exclusive_end(builder, end, inclusive)?;
    match proc.slots[base].ty {
        Ty::Array { len, width, .. } => {
            check_range(builder, start.clone(), exclusive.clone(), Value::Constant(len as i64))?;
            let count = Value::Temp(builder.build_binary(IrBinaryOp::Sub, exclusive, start.clone(), IrType::I16).ir()?);
            slice_from_array(builder, proc, frame, dest, base, start, count, width as i64)
        }
        Ty::Slice(_) => slice_from_slice(builder, proc, frame, dest, base, start, exclusive),
        _ => slice_from_vec(builder, proc, frame, dest, base, start, exclusive, elem),
    }
}

fn slice_from_array(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    array: usize,
    start: Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let empty = builder.new_label();
    let copy = builder.new_label();
    let is_empty = builder.build_binary(IrBinaryOp::Eq, count.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_empty), empty, copy).ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    let done = builder.new_label();
    builder.build_branch(done).ir()?;
    builder.create_block(copy).ir()?;
    let obj = alloc_vec(builder, count.clone(), count.clone(), width)?;
    copy_array_span(builder, frame, proc.slots[array].offset, start, &obj, count.clone(), width)?;
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    store_vec_ptr(builder, &header, &obj)?;
    store_cell(builder, frame, proc.slots[dest].offset + 2, Value::Constant(0))?;
    store_cell(builder, frame, proc.slots[dest].offset + 3, count)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn slice_from_slice(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    start: Value,
    exclusive: Value,
) -> Result<(), CompileError> {
    let base = proc.slots[src].offset;
    let old_len = load_cell(builder, frame, base + 3, IrType::I16)?;
    check_range(builder, start.clone(), exclusive.clone(), old_len)?;
    let count = Value::Temp(builder.build_binary(IrBinaryOp::Sub, exclusive, start.clone(), IrType::I16).ir()?);
    let old_start = load_cell(builder, frame, base + 2, IrType::I16)?;
    let new_start = Value::Temp(builder.build_binary(IrBinaryOp::Add, old_start, start, IrType::I16).ir()?);
    let empty = builder.new_label();
    let keep = builder.new_label();
    let done = builder.new_label();
    let is_empty = builder.build_binary(IrBinaryOp::Eq, count.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_empty), empty, keep).ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(keep).ir()?;
    for cell in 0..2 {
        let word = load_cell(builder, frame, base + cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    store_cell(builder, frame, proc.slots[dest].offset + 2, new_start)?;
    store_cell(builder, frame, proc.slots[dest].offset + 3, count)?;
    retain_vec(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn slice_from_vec(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
    start: Value,
    exclusive: Value,
    _elem: Ty,
) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let len_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let missing = builder.new_label();
    let present = builder.new_label();
    let join = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), missing, present).ir()?;
    builder.create_block(missing).ir()?;
    builder.build_store(Value::Constant(0), len_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(present).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    builder.build_store(len, len_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(join).ir()?;
    let len = Value::Temp(builder.build_load(len_slot, IrType::I16).ir()?);
    check_range(builder, start.clone(), exclusive.clone(), len)?;
    let count = Value::Temp(builder.build_binary(IrBinaryOp::Sub, exclusive, start.clone(), IrType::I16).ir()?);
    let empty = builder.new_label();
    let keep = builder.new_label();
    let done = builder.new_label();
    let is_empty = builder.build_binary(IrBinaryOp::Eq, count.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_empty), empty, keep).ir()?;
    builder.create_block(empty).ir()?;
    store_empty_slice(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(keep).ir()?;
    let header = vec_header(builder, proc, frame, vec)?;
    for cell in 0..2 {
        let word = load_cell(builder, &header, cell, IrType::I16)?;
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    store_cell(builder, frame, proc.slots[dest].offset + 2, start)?;
    store_cell(builder, frame, proc.slots[dest].offset + 3, count)?;
    retain_vec(builder, frame, proc.slots[dest].offset)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn copy_array_span(
    builder: &mut IrBuilder,
    frame: &Value,
    array_offset: i64,
    start: Value,
    obj: &Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    let i_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_store(Value::Constant(0), i_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let more = builder.build_binary(IrBinaryOp::Ult, i.clone(), count.clone(), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(more), body, done).ir()?;
    builder.create_block(body).ir()?;
    let from = Value::Temp(builder.build_binary(IrBinaryOp::Add, start.clone(), i.clone(), IrType::I16).ir()?);
    let src_scaled = scale_index(builder, from, width)?;
    let src_origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, Value::Constant(array_offset), src_scaled, IrType::I16).ir()?);
    let dst_scaled = scale_index(builder, i.clone(), width)?;
    let dst_origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, dst_scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?);
    for cell in 0..width {
        let src_off = Value::Temp(builder.build_binary(IrBinaryOp::Add, src_origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let src_ptr = builder.build_pointer_offset(frame.clone(), src_off, ptr_ty()).ir()?;
        let word = Value::Temp(builder.build_load(src_ptr, IrType::I16).ir()?);
        let dst_off = Value::Temp(builder.build_binary(IrBinaryOp::Add, dst_origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let dst_ptr = builder.build_pointer_offset(obj.clone(), dst_off, ptr_ty()).ir()?;
        builder.build_store(word, dst_ptr, IrType::I16).ir()?;
    }
    let next = builder.build_binary(IrBinaryOp::Add, i, Value::Constant(1), IrType::I16).ir()?;
    builder.build_store(Value::Temp(next), i_slot, IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_to_vec(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    match proc.slots[src].ty {
        Ty::Array { len, width, .. } => {
            if len == 0 {
                store_cell(builder, frame, proc.slots[dest].offset, Value::Constant(0))?;
                return store_cell(builder, frame, proc.slots[dest].offset + 1, Value::Constant(0));
            }
            let count = Value::Constant(len as i64);
            let obj = alloc_vec(builder, count.clone(), count.clone(), width as i64)?;
            copy_array_span(builder, frame, proc.slots[src].offset, Value::Constant(0), &obj, count, width as i64)?;
            store_vec_ptr(builder, &header, &obj)
        }
        Ty::Slice(_) => {
            let base = proc.slots[src].offset;
            let len = load_cell(builder, frame, base + 3, IrType::I16)?;
            let empty = builder.new_label();
            let copy = builder.new_label();
            let done = builder.new_label();
            let is_empty = builder.build_binary(IrBinaryOp::Eq, len.clone(), Value::Constant(0), IrType::I1).ir()?;
            builder.build_branch_cond(Value::Temp(is_empty), empty, copy).ir()?;
            builder.create_block(empty).ir()?;
            store_cell(builder, frame, proc.slots[dest].offset, Value::Constant(0))?;
            store_cell(builder, frame, proc.slots[dest].offset + 1, Value::Constant(0))?;
            builder.build_branch(done).ir()?;
            builder.create_block(copy).ir()?;
            let obj = alloc_vec(builder, len.clone(), len.clone(), elem.cells())?;
            let start = load_cell(builder, frame, base + 2, IrType::I16)?;
            let addr = load_cell(builder, frame, base, IrType::I16)?;
            let bank = load_cell(builder, frame, base + 1, IrType::I16)?;
            let src_obj = fat_from_parts(builder, addr, bank)?;
            copy_object_span(builder, &src_obj, start, &obj, len, elem.cells())?;
            store_vec_ptr(builder, &header, &obj)?;
            builder.build_branch(done).ir()?;
            builder.create_block(done).ir()?;
            Ok(())
        }
        _ => Ok(()),
    }
}

fn copy_object_span(
    builder: &mut IrBuilder,
    src: &Value,
    start: Value,
    dest: &Value,
    count: Value,
    width: i64,
) -> Result<(), CompileError> {
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    let i_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_store(Value::Constant(0), i_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let more = builder.build_binary(IrBinaryOp::Ult, i.clone(), count.clone(), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(more), body, done).ir()?;
    builder.create_block(body).ir()?;
    let from = Value::Temp(builder.build_binary(IrBinaryOp::Add, start.clone(), i.clone(), IrType::I16).ir()?);
    let src_scaled = scale_index(builder, from, width)?;
    let src_origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, src_scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?);
    let dst_scaled = scale_index(builder, i.clone(), width)?;
    let dst_origin = Value::Temp(builder.build_binary(IrBinaryOp::Add, dst_scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?);
    for cell in 0..width {
        let src_off = Value::Temp(builder.build_binary(IrBinaryOp::Add, src_origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let src_ptr = builder.build_pointer_offset(src.clone(), src_off, ptr_ty()).ir()?;
        let word = Value::Temp(builder.build_load(src_ptr, IrType::I16).ir()?);
        let dst_off = Value::Temp(builder.build_binary(IrBinaryOp::Add, dst_origin.clone(), Value::Constant(cell), IrType::I16).ir()?);
        let dst_ptr = builder.build_pointer_offset(dest.clone(), dst_off, ptr_ty()).ir()?;
        builder.build_store(word, dst_ptr, IrType::I16).ir()?;
    }
    let next = builder.build_binary(IrBinaryOp::Add, i, Value::Constant(1), IrType::I16).ir()?;
    builder.build_store(Value::Temp(next), i_slot, IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_vec_len(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, vec: usize) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { len, .. } => return store_slot(builder, proc, frame, dest, Value::Constant(len as i64)),
        Ty::Slice(_) => {
            let len = load_cell(builder, frame, proc.slots[vec].offset + 3, IrType::I16)?;
            return store_slot(builder, proc, frame, dest, len);
        }
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let empty = builder.new_label();
    let full = builder.new_label();
    let done = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), empty, full).ir()?;
    builder.create_block(empty).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(0))?;
    builder.build_branch(done).ir()?;
    builder.create_block(full).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    store_slot(builder, proc, frame, dest, len)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_vec_lit(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    elems: &[usize],
    elem: Ty,
) -> Result<(), CompileError> {
    let base = proc.slots[dest].offset;
    if elems.is_empty() {
        store_cell(builder, frame, base, Value::Constant(0))?;
        return store_cell(builder, frame, base + 1, Value::Constant(0));
    }
    let len = Value::Constant(elems.len() as i64);
    let obj = alloc_vec(builder, len.clone(), len, elem.cells())?;
    for (index, slot) in elems.iter().enumerate() {
        let at = VEC_DATA + index as i64 * elem.cells();
        for cell in 0..elem.cells() {
            let value = load_cell(builder, frame, proc.slots[*slot].offset + cell, IrType::I16)?;
            store_object_cell(builder, &obj, at + cell, value)?;
        }
    }
    let header = gep(builder, frame, base)?;
    store_vec_ptr(builder, &header, &obj)
}

fn emit_vec_index(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    vec: usize,
    index: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { .. } => return emit_array_index(builder, proc, frame, dest, vec, index),
        Ty::Slice(_) => return emit_slice_index(builder, proc, frame, dest, vec, index, elem),
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bad = builder.new_label();
    let ok_ptr = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), bad, ok_ptr).ir()?;
    builder.create_block(ok_ptr).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1).ir()?;
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    copy_object_cells(builder, proc, frame, &obj, Value::Temp(data), dest, elem.cells())
}

fn emit_vec_empty(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, vec: usize) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { len, .. } => {
            return store_slot(builder, proc, frame, dest, Value::Constant(i64::from(len == 0)));
        }
        Ty::Slice(_) => {
            let len = load_cell(builder, frame, proc.slots[vec].offset + 3, IrType::I16)?;
            let empty = builder.build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1).ir()?;
            return store_slot(builder, proc, frame, dest, Value::Temp(empty));
        }
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let yes = builder.new_label();
    let check = builder.new_label();
    let no = builder.new_label();
    let done = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), yes, check).ir()?;
    builder.create_block(check).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let zero = builder.build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(zero), yes, no).ir()?;
    builder.create_block(yes).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(1))?;
    builder.build_branch(done).ir()?;
    builder.create_block(no).ir()?;
    store_slot(builder, proc, frame, dest, Value::Constant(0))?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_vec_clear(builder: &mut IrBuilder, proc: &Proc, frame: &Value, vec: usize) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    adjust_refcount(builder, addr, bank, false)?;
    store_cell(builder, &header, 0, Value::Constant(0))?;
    store_cell(builder, &header, 1, Value::Constant(0))
}

fn emit_vec_index_store(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    index: usize,
    value: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    if let Ty::Array { .. } = proc.slots[vec].ty {
        return emit_array_store(builder, proc, frame, vec, index, value);
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bad = builder.new_label();
    let have = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), bad, have).ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder.build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1).ir()?;
    let write = builder.new_label();
    let detach = builder.new_label();
    builder.build_branch_cond(Value::Temp(unique), write, detach).ir()?;
    builder.create_block(detach).ir()?;
    detach_vec(builder, &header, &obj, elem.cells())?;
    builder.build_branch(write).ir()?;
    builder.create_block(write).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1).ir()?;
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, bad).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    store_object_cells(builder, proc, frame, value, &obj, Value::Temp(data), elem.cells())
}

fn store_immediate(builder: &mut IrBuilder, frame: &Value, dest: usize, proc: &Proc, tag: u16) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, Value::Constant(tag as i64))?;
    store_cell(builder, frame, base + 1, Value::Constant(0))?;
    store_cell(builder, frame, base + 2, Value::Constant(0))
}

fn emit_vec_get(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    index: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    match proc.slots[vec].ty {
        Ty::Array { .. } => return emit_array_get(builder, proc, frame, vec, index, value, dest, elem, some_tag, none_tag),
        Ty::Slice(_) => return emit_slice_get(builder, proc, frame, vec, index, value, dest, elem, some_tag, none_tag),
        _ => {}
    }
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let none = builder.new_label();
    let have = builder.new_label();
    let done = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), none, have).ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let at = load_slot(builder, proc, frame, index)?;
    let in_range = builder.build_binary(IrBinaryOp::Ult, at.clone(), len, IrType::I1).ir()?;
    let ok = builder.new_label();
    builder.build_branch_cond(Value::Temp(in_range), ok, none).ir()?;
    builder.create_block(ok).ir()?;
    let scaled = scale_index(builder, at, elem.cells())?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    copy_object_cells(builder, proc, frame, &obj, Value::Temp(data), value, elem.cells())?;
    emit_make(builder, proc, frame, dest, some_tag, &[(value, elem.cells())])?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_vec_pop(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    value: usize,
    dest: usize,
    elem: Ty,
    some_tag: u16,
    none_tag: u16,
) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let none = builder.new_label();
    let have = builder.new_label();
    let done = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), none, have).ir()?;
    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let empty = builder.build_binary(IrBinaryOp::Eq, len, Value::Constant(0), IrType::I1).ir()?;
    let live = builder.new_label();
    builder.build_branch_cond(Value::Temp(empty), none, live).ir()?;
    builder.create_block(live).ir()?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder.build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1).ir()?;
    let write = builder.new_label();
    let detach = builder.new_label();
    builder.build_branch_cond(Value::Temp(unique), write, detach).ir()?;
    builder.create_block(detach).ir()?;
    detach_vec(builder, &header, &obj, elem.cells())?;
    builder.build_branch(write).ir()?;
    builder.create_block(write).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let next = builder.build_binary(IrBinaryOp::Sub, len, Value::Constant(1), IrType::I16).ir()?;
    store_object_cell(builder, &obj, VEC_LEN, Value::Temp(next))?;
    let scaled = scale_index(builder, Value::Temp(next), elem.cells())?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    copy_object_cells(builder, proc, frame, &obj, Value::Temp(data), value, elem.cells())?;
    emit_make(builder, proc, frame, dest, some_tag, &[(value, elem.cells())])?;
    builder.build_branch(done).ir()?;
    builder.create_block(none).ir()?;
    store_immediate(builder, frame, dest, proc, none_tag)?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn emit_vec_push(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    vec: usize,
    value: usize,
    elem: Ty,
) -> Result<(), CompileError> {
    let header = vec_header(builder, proc, frame, vec)?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let fresh = builder.new_label();
    let have = builder.new_label();
    let append = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), fresh, have).ir()?;

    builder.create_block(fresh).ir()?;
    let obj = alloc_vec(builder, Value::Constant(0), Value::Constant(4), elem.cells())?;
    store_vec_ptr(builder, &header, &obj)?;
    builder.build_branch(append).ir()?;

    builder.create_block(have).ir()?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, VEC_REFS, IrType::I16)?;
    let unique = builder.build_binary(IrBinaryOp::Eq, refs, Value::Constant(1), IrType::I1).ir()?;
    let detach = builder.new_label();
    let owned = builder.new_label();
    builder.build_branch_cond(Value::Temp(unique), owned, detach).ir()?;

    builder.create_block(detach).ir()?;
    detach_vec(builder, &header, &obj, elem.cells())?;
    builder.build_branch(append).ir()?;

    builder.create_block(owned).ir()?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let cap = load_cell(builder, &obj, VEC_CAP, IrType::I16)?;
    let full = builder.build_binary(IrBinaryOp::Uge, len, cap.clone(), IrType::I1).ir()?;
    let grow = builder.new_label();
    builder.build_branch_cond(Value::Temp(full), grow, append).ir()?;
    builder.create_block(grow).ir()?;
    let new_cap = builder.build_binary(IrBinaryOp::Add, cap, Value::Constant(4), IrType::I16).ir()?;
    grow_vec(builder, &header, &obj, Value::Temp(new_cap), elem.cells())?;
    builder.build_branch(append).ir()?;

    builder.create_block(append).ir()?;
    let addr = load_cell(builder, &header, 0, IrType::I16)?;
    let bank = load_cell(builder, &header, 1, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let len = load_cell(builder, &obj, VEC_LEN, IrType::I16)?;
    let scaled = scale_index(builder, len.clone(), elem.cells())?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    store_object_cells(builder, proc, frame, value, &obj, Value::Temp(data), elem.cells())?;
    let next = builder.build_binary(IrBinaryOp::Add, len, Value::Constant(1), IrType::I16).ir()?;
    store_object_cell(builder, &obj, VEC_LEN, Value::Temp(next))
}

fn detach_vec(builder: &mut IrBuilder, header: &Value, obj: &Value, cells: i64) -> Result<(), CompileError> {
    let len = load_cell(builder, obj, VEC_LEN, IrType::I16)?;
    let cap = load_cell(builder, obj, VEC_CAP, IrType::I16)?;
    let room = builder.build_binary(IrBinaryOp::Ugt, cap.clone(), len.clone(), IrType::I1).ir()?;
    let use_cap = builder.new_label();
    let bump = builder.new_label();
    let join = builder.new_label();
    let saved = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_branch_cond(Value::Temp(room), use_cap, bump).ir()?;
    builder.create_block(use_cap).ir()?;
    builder.build_store(cap, saved.clone(), IrType::I16).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(bump).ir()?;
    let bumped = builder.build_binary(IrBinaryOp::Add, len.clone(), Value::Constant(4), IrType::I16).ir()?;
    builder.build_store(Value::Temp(bumped), saved.clone(), IrType::I16).ir()?;
    builder.build_branch(join).ir()?;
    builder.create_block(join).ir()?;
    let new_cap = Value::Temp(builder.build_load(saved, IrType::I16).ir()?);
    let new_obj = alloc_vec(builder, len.clone(), new_cap, cells)?;
    copy_elements(builder, obj, &new_obj, len, cells)?;
    let refs = load_cell(builder, obj, VEC_REFS, IrType::I16)?;
    let next = builder.build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16).ir()?;
    store_object_cell(builder, obj, VEC_REFS, Value::Temp(next))?;
    store_vec_ptr(builder, header, &new_obj)
}

fn grow_vec(builder: &mut IrBuilder, header: &Value, obj: &Value, new_cap: Value, cells: i64) -> Result<(), CompileError> {
    let len = load_cell(builder, obj, VEC_LEN, IrType::I16)?;
    let new_obj = alloc_vec(builder, len.clone(), new_cap, cells)?;
    copy_elements(builder, obj, &new_obj, len, cells)?;
    builder.build_call(Value::Global("free".to_string()), vec![obj.clone()], IrType::Void).ir()?;
    store_vec_ptr(builder, header, &new_obj)
}

fn alloc_vec(builder: &mut IrBuilder, len: Value, cap: Value, cells: i64) -> Result<Value, CompileError> {
    let scaled = scale_index(builder, cap.clone(), cells)?;
    let words = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    let size = as_i32(builder, Value::Temp(words))?;
    let raw = builder.build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty()).ir()?.unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    store_object_cell(builder, &obj, VEC_REFS, Value::Constant(1))?;
    store_object_cell(builder, &obj, VEC_LEN, len)?;
    store_object_cell(builder, &obj, VEC_CAP, cap)?;
    Ok(obj)
}

fn store_vec_ptr(builder: &mut IrBuilder, header: &Value, obj: &Value) -> Result<(), CompileError> {
    let addr = low16_of_ptr(builder, obj)?;
    let bank = bank16_of_ptr(builder, obj)?;
    store_cell(builder, header, 0, addr)?;
    store_cell(builder, header, 1, bank)
}

fn scale_index(builder: &mut IrBuilder, index: Value, cells: i64) -> Result<Value, CompileError> {
    if cells == 1 {
        return Ok(index);
    }
    let scaled = builder.build_binary(IrBinaryOp::Mul, index, Value::Constant(cells), IrType::I16).ir()?;
    Ok(Value::Temp(scaled))
}

fn copy_elements(builder: &mut IrBuilder, src: &Value, dest: &Value, len: Value, cells: i64) -> Result<(), CompileError> {
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    let i_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_store(Value::Constant(0), i_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let i = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let more = builder.build_binary(IrBinaryOp::Ult, i.clone(), len.clone(), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(more), body, done).ir()?;
    builder.create_block(body).ir()?;
    let scaled = scale_index(builder, i.clone(), cells)?;
    let data = builder.build_binary(IrBinaryOp::Add, scaled, Value::Constant(VEC_DATA), IrType::I16).ir()?;
    for cell in 0..cells {
        let at = builder.build_binary(IrBinaryOp::Add, Value::Temp(data), Value::Constant(cell), IrType::I16).ir()?;
        let from = builder.build_pointer_offset(src.clone(), Value::Temp(at), ptr_ty()).ir()?;
        let word = Value::Temp(builder.build_load(from, IrType::I16).ir()?);
        let into = builder.build_pointer_offset(dest.clone(), Value::Temp(at), ptr_ty()).ir()?;
        builder.build_store(word, into, IrType::I16).ir()?;
    }
    let next = builder.build_binary(IrBinaryOp::Add, i, Value::Constant(1), IrType::I16).ir()?;
    builder.build_store(Value::Temp(next), i_slot, IrType::I16).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn store_object_cells(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    src: usize,
    obj: &Value,
    data: Value,
    cells: i64,
) -> Result<(), CompileError> {
    for cell in 0..cells {
        let value = load_cell(builder, frame, proc.slots[src].offset + cell, IrType::I16)?;
        let at = builder.build_binary(IrBinaryOp::Add, data.clone(), Value::Constant(cell), IrType::I16).ir()?;
        let into = builder.build_pointer_offset(obj.clone(), Value::Temp(at), ptr_ty()).ir()?;
        builder.build_store(value, into, IrType::I16).ir()?;
    }
    Ok(())
}

fn copy_object_cells(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    obj: &Value,
    data: Value,
    dest: usize,
    cells: i64,
) -> Result<(), CompileError> {
    for cell in 0..cells {
        let at = builder.build_binary(IrBinaryOp::Add, data.clone(), Value::Constant(cell), IrType::I16).ir()?;
        let from = builder.build_pointer_offset(obj.clone(), Value::Temp(at), ptr_ty()).ir()?;
        let word = Value::Temp(builder.build_load(from, IrType::I16).ir()?);
        store_cell(builder, frame, proc.slots[dest].offset + cell, word)?;
    }
    Ok(())
}

fn emit_make(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    tag: u16,
    fields: &[(usize, i64)],
) -> Result<(), CompileError> {
    // Keep each field alive before `dest` is overwritten. `x = Cons(1, x)` retains
    // the old node, copies it into the new one, then drops the slot's own share.
    for (slot, _) in fields {
        if keeps(proc, proc.slots[*slot].ty) {
            retain_managed(builder, proc, frame, *slot)?;
        }
    }
    if fields.is_empty() {
        release_managed(builder, proc, frame, dest)?;
        let base = proc.slots[dest].offset;
        store_cell(builder, frame, base, Value::Constant(tag as i64))?;
        store_cell(builder, frame, base + 1, Value::Constant(0))?;
        return store_cell(builder, frame, base + 2, Value::Constant(0));
    }
    let words = 2 + fields.iter().map(|(_, cells)| *cells).sum::<i64>();
    let size = i32_const(builder, words)?;
    let raw = builder.build_call(Value::Global("malloc".to_string()), vec![size], ptr_ty()).ir()?.unwrap();
    let obj = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &obj)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    builder.build_call(Value::Global("rfl_fail".to_string()), Vec::new(), IrType::Void).ir()?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Constant(1))?;
    store_object_cell(builder, &obj, OBJ_TAG, Value::Constant(tag as i64))?;
    let mut offset = OBJ_FIELDS;
    for (slot, cells) in fields {
        let src = proc.slots[*slot].offset;
        for index in 0..*cells {
            let value = load_cell(builder, frame, src + index, IrType::I16)?;
            store_object_cell(builder, &obj, offset, value)?;
            offset += 1;
        }
    }
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    release_managed(builder, proc, frame, dest)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, Value::Constant(tag as i64))?;
    store_cell(builder, frame, base + 1, addr)?;
    store_cell(builder, frame, base + 2, bank)
}

fn emit_read(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    src: usize,
    offset: i64,
    cells: i64,
) -> Result<(), CompileError> {
    let src_at = proc.slots[src].offset;
    let addr = load_cell(builder, frame, src_at + 1, IrType::I16)?;
    let bank = load_cell(builder, frame, src_at + 2, IrType::I16)?;
    let obj = fat_from_parts(builder, addr, bank)?;
    let mut words = Vec::new();
    for index in 0..cells {
        let ptr = gep(builder, &obj, offset + index)?;
        words.push(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?));
    }
    let ty = proc.slots[dest].ty;
    if ty.is_managed() {
        retain_words(builder, &words, ty)?;
        release_managed(builder, proc, frame, dest)?;
    } else if keeps(proc, ty) {
        release_managed(builder, proc, frame, dest)?;
    }
    let dest_at = proc.slots[dest].offset;
    for (index, value) in words.into_iter().enumerate() {
        store_cell(builder, frame, dest_at + index as i64, value)?;
    }
    if keeps(proc, ty) && !ty.is_managed() {
        retain_tree(builder, proc, frame, dest_at, ty)?;
    }
    Ok(())
}

fn store_object_cell(builder: &mut IrBuilder, obj: &Value, offset: i64, value: Value) -> Result<(), CompileError> {
    let ptr = gep(builder, obj, offset)?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

fn fat_from_parts(builder: &mut IrBuilder, addr: Value, bank: Value) -> Result<Value, CompileError> {
    let mem = builder.build_alloca(IrType::I32, None).ir()?;
    let low = builder.build_pointer_offset_with_bank(mem.clone(), Value::Constant(0), ptr_ty(), Some(BankTag::Stack)).ir()?;
    builder.build_store(addr, low, IrType::I16).ir()?;
    let high = builder.build_pointer_offset_with_bank(mem.clone(), Value::Constant(1), ptr_ty(), Some(BankTag::Stack)).ir()?;
    builder.build_store(bank, high, IrType::I16).ir()?;
    let bits = builder.build_load(mem, IrType::I32).ir()?;
    let raw = builder.build_unary(IrUnaryOp::IntToPtr, Value::Temp(bits), ptr_ty()).ir()?;
    Ok(fat_temp(raw, BankTag::Mixed))
}

fn copy_slot(
    builder: &mut IrBuilder,
    src_proc: &Proc,
    src_frame: &Value,
    src: usize,
    dest_proc: &Proc,
    dest_frame: &Value,
    dest: usize,
) -> Result<(), CompileError> {
    let ty = src_proc.slots[src].ty;
    if ty == Ty::Unit {
        return Ok(());
    }
    if cell_wise(ty) {
        for index in 0..ty.cells() {
            let value = load_cell(builder, src_frame, src_proc.slots[src].offset + index, IrType::I16)?;
            store_cell(builder, dest_frame, dest_proc.slots[dest].offset + index, value)?;
        }
        if keeps(src_proc, ty) {
            retain_tree(builder, src_proc, dest_frame, dest_proc.slots[dest].offset, ty)?;
        }
        return Ok(());
    }
    let value = load_slot(builder, src_proc, src_frame, src)?;
    store_slot(builder, dest_proc, dest_frame, dest, value)
}

fn load_slot(builder: &mut IrBuilder, proc: &Proc, frame: &Value, slot: usize) -> Result<Value, CompileError> {
    let slot = &proc.slots[slot];
    if slot.ty == Ty::Unit {
        return Ok(Value::Constant(0));
    }
    load_cell(builder, frame, slot.offset, ir_ty(slot.ty))
}

fn store_slot(builder: &mut IrBuilder, proc: &Proc, frame: &Value, slot: usize, value: Value) -> Result<(), CompileError> {
    let slot = &proc.slots[slot];
    if slot.ty == Ty::Unit {
        return Ok(());
    }
    let ptr = gep(builder, frame, slot.offset)?;
    builder.build_store(value, ptr, ir_ty(slot.ty)).ir()
}

fn load_cell(builder: &mut IrBuilder, frame: &Value, offset: i64, ty: IrType) -> Result<Value, CompileError> {
    let ptr = gep(builder, frame, offset)?;
    Ok(Value::Temp(builder.build_load(ptr, ty).ir()?))
}

fn store_cell(builder: &mut IrBuilder, frame: &Value, offset: i64, value: Value) -> Result<(), CompileError> {
    let ptr = gep(builder, frame, offset)?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

fn load_fat(builder: &mut IrBuilder, frame: &Value, offset: i64) -> Result<Value, CompileError> {
    let ptr = gep(builder, frame, offset)?;
    let temp = builder.build_load(ptr, ptr_ty()).ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

fn load_ptr_slot(builder: &mut IrBuilder, slot: &Value) -> Result<Value, CompileError> {
    let temp = builder.build_load(slot.clone(), ptr_ty()).ir()?;
    Ok(fat_temp(temp, BankTag::Mixed))
}

fn gep(builder: &mut IrBuilder, frame: &Value, offset: i64) -> Result<Value, CompileError> {
    builder.build_pointer_offset(frame.clone(), Value::Constant(offset), ptr_ty()).ir()
}

fn widen_for_op(builder: &mut IrBuilder, value: Value, ty: Ty) -> Result<Value, CompileError> {
    match ty {
        Ty::I8 => sign_extend_i8(builder, value),
        Ty::U8 => Ok(Value::Temp(builder.build_unary(IrUnaryOp::ZExt, value, IrType::I16).ir()?)),
        _ => Ok(value),
    }
}

fn cast_value(builder: &mut IrBuilder, value: Value, from: Ty, to: Ty) -> Result<Value, CompileError> {
    if from == to || (ir_ty(from) == ir_ty(to) && from.cells() == to.cells() && width(from) == width(to)) {
        return Ok(value);
    }
    let mut value = value;
    if width(from) < 16 {
        value = if from.is_signed() { sign_extend_i8(builder, value)? } else {
            Value::Temp(builder.build_unary(IrUnaryOp::ZExt, value, IrType::I16).ir()?)
        };
    }
    if width(to) < width(from) || (width(from) < 16 && width(to) <= 16 && matches!(to, Ty::I8 | Ty::U8)) {
        if matches!(to, Ty::I8 | Ty::U8) {
            let low = if width(from) > 16 {
                Value::Temp(builder.build_unary(IrUnaryOp::Trunc, value, IrType::I16).ir()?)
            } else {
                value
            };
            return Ok(Value::Temp(builder.build_unary(IrUnaryOp::Trunc, low, IrType::I8).ir()?));
        }
        return Ok(Value::Temp(builder.build_unary(IrUnaryOp::Trunc, value, ir_ty(to)).ir()?));
    }
    if width(to) > 16 && width(from) <= 16 {
        let op = if from.is_signed() { IrUnaryOp::SExt } else { IrUnaryOp::ZExt };
        let mid = if width(to) == 64 && width(from) == 16 {
            value
        } else if width(to) == 64 {
            Value::Temp(builder.build_unary(op, value, IrType::I32).ir()?)
        } else {
            value
        };
        let op = if from.is_signed() { IrUnaryOp::SExt } else { IrUnaryOp::ZExt };
        return Ok(Value::Temp(builder.build_unary(op, mid, ir_ty(to)).ir()?));
    }
    if width(to) > width(from) {
        let op = if from.is_signed() { IrUnaryOp::SExt } else { IrUnaryOp::ZExt };
        return Ok(Value::Temp(builder.build_unary(op, value, ir_ty(to)).ir()?));
    }
    Ok(Value::Temp(builder.build_unary(IrUnaryOp::Trunc, value, ir_ty(to)).ir()?))
}

fn sign_extend_i8(builder: &mut IrBuilder, value: Value) -> Result<Value, CompileError> {
    let wide = builder.build_unary(IrUnaryOp::ZExt, value, IrType::I16).ir()?;
    let shifted = builder.build_binary(IrBinaryOp::Shl, Value::Temp(wide), Value::Constant(8), IrType::I16).ir()?;
    Ok(Value::Temp(builder.build_binary(IrBinaryOp::AShr, Value::Temp(shifted), Value::Constant(8), IrType::I16).ir()?))
}

fn emit_print(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    slot: usize,
    ty: Ty,
    newline: bool,
) -> Result<(), CompileError> {
    if ty == Ty::Bool {
        let value = load_slot(builder, proc, frame, slot)?;
        let yes = builder.new_label();
        let no = builder.new_label();
        let after = builder.new_label();
        let is_false = builder.build_binary(IrBinaryOp::Eq, value, Value::Constant(0), IrType::I1).ir()?;
        builder.build_branch_cond(Value::Temp(is_false), no, yes).ir()?;
        builder.create_block(yes).ir()?;
        emit_chars(builder, &['t' as u16, 'r' as u16, 'u' as u16, 'e' as u16])?;
        builder.build_branch(after).ir()?;
        builder.create_block(no).ir()?;
        emit_chars(builder, &['f' as u16, 'a' as u16, 'l' as u16, 's' as u16, 'e' as u16])?;
        builder.build_branch(after).ir()?;
        builder.create_block(after).ir()?;
        if newline {
            putchar(builder, Value::Constant(10))?;
        }
        return Ok(());
    }
    let value = load_slot(builder, proc, frame, slot)?;
    let signed = ty.is_signed();
    let bits = cast_value(builder, value, ty, if signed { Ty::I64 } else { Ty::U64 })?;
    if signed {
        let zero = wide_zero(builder, IrType::I64)?;
        let neg = builder.build_binary(IrBinaryOp::Slt, bits.clone(), zero.clone(), IrType::I1).ir()?;
        let minus = builder.new_label();
        let plain = builder.new_label();
        let after = builder.new_label();
        builder.build_branch_cond(Value::Temp(neg), minus, plain).ir()?;
        builder.create_block(minus).ir()?;
        putchar(builder, Value::Constant('-' as i64))?;
        let flipped = builder.build_binary(IrBinaryOp::Sub, zero, bits.clone(), IrType::I64).ir()?;
        builder.build_call(Value::Global("rfl_print_u64".to_string()), vec![Value::Temp(flipped)], IrType::Void).ir()?;
        builder.build_branch(after).ir()?;
        builder.create_block(plain).ir()?;
        builder.build_call(Value::Global("rfl_print_u64".to_string()), vec![bits], IrType::Void).ir()?;
        builder.build_branch(after).ir()?;
        builder.create_block(after).ir()?;
    } else {
        builder.build_call(Value::Global("rfl_print_u64".to_string()), vec![bits], IrType::Void).ir()?;
    }
    if newline {
        putchar(builder, Value::Constant(10))?;
    }
    Ok(())
}

fn emit_chars(builder: &mut IrBuilder, chars: &[u16]) -> Result<(), CompileError> {
    for ch in chars {
        putchar(builder, Value::Constant(*ch as i64))?;
    }
    Ok(())
}

fn putchar(builder: &mut IrBuilder, value: Value) -> Result<(), CompileError> {
    builder.build_call(Value::Global("putchar".to_string()), vec![value], IrType::Void).ir()?;
    Ok(())
}

/// Unsigned decimal. The caller prints a minus and passes the magnitude.
fn emit_print_u64(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_print_u64".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I64);
    let entry = builder.new_label();
    let step = builder.new_label();
    let dump = builder.new_label();
    let emit_digit = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let buf = builder.build_alloca(IrType::I16, Some(Value::Constant(24))).ir()?;
    let n_slot = builder.build_alloca(IrType::I64, None).ir()?;
    let i_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_store(Value::Temp(0), n_slot.clone(), IrType::I64).ir()?;
    builder.build_store(Value::Constant(0), i_slot.clone(), IrType::I16).ir()?;
    builder.build_branch(step).ir()?;

    builder.create_block(step).ir()?;
    let n = Value::Temp(builder.build_load(n_slot.clone(), IrType::I64).ir()?);
    let ten = i64_const(builder, 10)?;
    let digit = Value::Temp(builder.build_binary(IrBinaryOp::URem, n.clone(), ten.clone(), IrType::I64).ir()?);
    let next = Value::Temp(builder.build_binary(IrBinaryOp::UDiv, n, ten, IrType::I64).ir()?);
    builder.build_store(next.clone(), n_slot.clone(), IrType::I64).ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let cell = builder.build_pointer_offset(buf.clone(), index.clone(), ptr_ty()).ir()?;
    let low = Value::Temp(builder.build_unary(IrUnaryOp::Trunc, digit, IrType::I16).ir()?);
    builder.build_store(low, cell, IrType::I16).ir()?;
    let bumped = Value::Temp(builder.build_binary(IrBinaryOp::Add, index, Value::Constant(1), IrType::I16).ir()?);
    builder.build_store(bumped, i_slot.clone(), IrType::I16).ir()?;
    let zero = wide_zero(builder, IrType::I64)?;
    let finished = Value::Temp(builder.build_binary(IrBinaryOp::Eq, next, zero, IrType::I1).ir()?);
    builder.build_branch_cond(finished, dump, step).ir()?;

    builder.create_block(dump).ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let empty = Value::Temp(builder.build_binary(IrBinaryOp::Eq, index, Value::Constant(0), IrType::I1).ir()?);
    builder.build_branch_cond(empty, done, emit_digit).ir()?;

    builder.create_block(emit_digit).ir()?;
    let index = Value::Temp(builder.build_load(i_slot.clone(), IrType::I16).ir()?);
    let prev = Value::Temp(builder.build_binary(IrBinaryOp::Sub, index, Value::Constant(1), IrType::I16).ir()?);
    builder.build_store(prev.clone(), i_slot.clone(), IrType::I16).ir()?;
    let cell = builder.build_pointer_offset(buf, prev, ptr_ty()).ir()?;
    let digit = Value::Temp(builder.build_load(cell, IrType::I16).ir()?);
    let ch = Value::Temp(builder.build_binary(IrBinaryOp::Add, digit, Value::Constant(48), IrType::I16).ir()?);
    putchar(builder, ch)?;
    builder.build_branch(dump).ir()?;

    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

fn i64_const(builder: &mut IrBuilder, value: i64) -> Result<Value, CompileError> {
    let temp = builder.build_binary(IrBinaryOp::Add, Value::Constant(value), Value::Constant(0), IrType::I64).ir()?;
    Ok(Value::Temp(temp))
}

fn i32_const(builder: &mut IrBuilder, value: i64) -> Result<Value, CompileError> {
    let temp = builder.build_binary(IrBinaryOp::Add, Value::Constant(value), Value::Constant(0), IrType::I32).ir()?;
    Ok(Value::Temp(temp))
}

fn wide_zero(builder: &mut IrBuilder, ty: IrType) -> Result<Value, CompileError> {
    if matches!(ty, IrType::I32 | IrType::I64) {
        let temp = builder.build_binary(IrBinaryOp::Add, Value::Constant(0), Value::Constant(0), ty).ir()?;
        Ok(Value::Temp(temp))
    } else {
        Ok(Value::Constant(0))
    }
}

fn arith_ty(ty: Ty) -> IrType {
    match ty {
        Ty::I8 | Ty::U8 | Ty::I16 | Ty::U16 | Ty::Usize => IrType::I16,
        Ty::I32 | Ty::U32 => IrType::I32,
        Ty::I64 | Ty::U64 => IrType::I64,
        Ty::Bool | Ty::Unit | Ty::Adt(_) | Ty::Func(_) | Ty::Vec(_) | Ty::Ref(_) | Ty::Array { .. } | Ty::Slice(_) | Ty::Struct { .. } | Ty::Tuple { .. } => IrType::I16,
    }
}

fn ir_op(op: BinOp, ty: Ty) -> IrBinaryOp {
    let unsigned = !ty.is_signed();
    match op {
        BinOp::Add => IrBinaryOp::Add,
        BinOp::Sub => IrBinaryOp::Sub,
        BinOp::Mul => IrBinaryOp::Mul,
        BinOp::Div => if unsigned { IrBinaryOp::UDiv } else { IrBinaryOp::SDiv },
        BinOp::Rem => if unsigned { IrBinaryOp::URem } else { IrBinaryOp::SRem },
        BinOp::Eq => IrBinaryOp::Eq,
        BinOp::Ne => IrBinaryOp::Ne,
        BinOp::Lt => if unsigned { IrBinaryOp::Ult } else { IrBinaryOp::Slt },
        BinOp::Le => if unsigned { IrBinaryOp::Ule } else { IrBinaryOp::Sle },
        BinOp::Gt => if unsigned { IrBinaryOp::Ugt } else { IrBinaryOp::Sgt },
        BinOp::Ge => if unsigned { IrBinaryOp::Uge } else { IrBinaryOp::Sge },
        BinOp::And | BinOp::Or => IrBinaryOp::And,
    }
}

fn ir_ty(ty: Ty) -> IrType {
    match ty {
        Ty::I8 | Ty::U8 => IrType::I8,
        Ty::I16 | Ty::U16 | Ty::Usize => IrType::I16,
        Ty::I32 | Ty::U32 => IrType::I32,
        Ty::I64 | Ty::U64 => IrType::I64,
        Ty::Bool => IrType::I1,
        Ty::Unit => IrType::Void,
        Ty::Adt(_) | Ty::Func(_) | Ty::Vec(_) | Ty::Ref(_) | Ty::Array { .. } | Ty::Slice(_) | Ty::Struct { .. } | Ty::Tuple { .. } => IrType::I16,
    }
}

fn width(ty: Ty) -> u32 {
    match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 | Ty::Usize | Ty::Bool => 16,
        Ty::I32 | Ty::U32 => 32,
        Ty::I64 | Ty::U64 => 64,
        Ty::Unit | Ty::Adt(_) | Ty::Func(_) | Ty::Vec(_) | Ty::Ref(_) | Ty::Array { .. } | Ty::Slice(_) | Ty::Struct { .. } | Ty::Tuple { .. } => 0,
    }
}

fn cell_wise(ty: Ty) -> bool {
    matches!(ty, Ty::Adt(_) | Ty::Func(_) | Ty::Vec(_) | Ty::Ref(_) | Ty::Array { .. } | Ty::Slice(_) | Ty::Struct { .. } | Ty::Tuple { .. })
}

/// Pointer to the two cells that store a vector's buffer address and bank.
fn vec_header(builder: &mut IrBuilder, proc: &Proc, frame: &Value, slot: usize) -> Result<Value, CompileError> {
    let offset = proc.slots[slot].offset;
    if proc.slots[slot].ty.is_ref() {
        let addr = load_cell(builder, frame, offset, IrType::I16)?;
        let bank = load_cell(builder, frame, offset + 1, IrType::I16)?;
        fat_from_parts(builder, addr, bank)
    } else {
        gep(builder, frame, offset)
    }
}

fn emit_try(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    src: usize,
    dest: usize,
    result: usize,
    ok_tag: u16,
    fail_tag: u16,
    payload: Ty,
    fail_slot: Option<usize>,
    fail: Option<Ty>,
    release: bool,
) -> Result<(), CompileError> {
    let tag = load_cell(builder, frame, proc.slots[src].offset, IrType::I16)?;
    let ok = builder.new_label();
    let fail_block = builder.new_label();
    let is_ok = builder.build_binary(IrBinaryOp::Eq, tag, Value::Constant(ok_tag as i64), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_ok), ok, fail_block).ir()?;
    builder.create_block(fail_block).ir()?;
    if let Some(fail_ty) = fail {
        let slot = fail_slot.expect("a payload failure has a slot");
        emit_read(builder, proc, frame, slot, src, OBJ_FIELDS, fail_ty.cells())?;
        emit_make(builder, proc, frame, result, fail_tag, &[(slot, fail_ty.cells())])?;
    } else {
        store_immediate(builder, frame, result, proc, fail_tag)?;
    }
    emit_return(builder, proc, frame, release)?;
    builder.create_block(ok).ir()?;
    emit_read(builder, proc, frame, dest, src, OBJ_FIELDS, payload.cells())
}

fn emit_borrow(builder: &mut IrBuilder, proc: &Proc, frame: &Value, dest: usize, slot: usize) -> Result<(), CompileError> {
    let place = gep(builder, frame, proc.slots[slot].offset)?;
    let addr = low16_of_ptr(builder, &place)?;
    let bank = bank16_of_ptr(builder, &place)?;
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, addr)?;
    store_cell(builder, frame, base + 1, bank)
}

fn ptr_ty() -> IrType {
    IrType::FatPtr(Box::new(IrType::I16))
}

fn retain_value_at(builder: &mut IrBuilder, frame: &Value, offset: i64, ty: Ty) -> Result<(), CompileError> {
    let index = match ty {
        Ty::Vec(_) | Ty::Slice(_) => 0,
        Ty::Adt(_) => 1,
        Ty::Func(_) => 2,
        _ => return Ok(()),
    };
    let addr = load_cell(builder, frame, offset + index, IrType::I16)?;
    let bank = load_cell(builder, frame, offset + index + 1, IrType::I16)?;
    adjust_refcount(builder, addr, bank, true)
}

fn retain_managed(builder: &mut IrBuilder, proc: &Proc, frame: &Value, slot: usize) -> Result<(), CompileError> {
    let offset = proc.slots[slot].offset;
    retain_tree(builder, proc, frame, offset, proc.slots[slot].ty)
}

fn retain_words(builder: &mut IrBuilder, words: &[Value], ty: Ty) -> Result<(), CompileError> {
    let index = match ty {
        Ty::Vec(_) | Ty::Slice(_) => 0,
        Ty::Adt(_) => 1,
        Ty::Func(_) => 2,
        _ => return Ok(()),
    };
    adjust_refcount(builder, words[index].clone(), words[index + 1].clone(), true)
}

fn load_through(builder: &mut IrBuilder, slot: &Value, index: i64) -> Result<Value, CompileError> {
    let ptr = builder.build_pointer_offset(slot.clone(), Value::Constant(index), ptr_ty()).ir()?;
    Ok(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?))
}

fn store_through(builder: &mut IrBuilder, slot: &Value, index: i64, value: Value) -> Result<(), CompileError> {
    let ptr = builder.build_pointer_offset(slot.clone(), Value::Constant(index), ptr_ty()).ir()?;
    builder.build_store(value, ptr, IrType::I16).ir()
}

fn retain_managed_through(builder: &mut IrBuilder, proc: &Proc, slot: &Value, ty: Ty) -> Result<(), CompileError> {
    match ty {
        Ty::Struct { id, .. } => through_fields(builder, proc, slot, &proc.structs[id as usize], true),
        Ty::Tuple { id, .. } => through_fields(builder, proc, slot, &proc.tuples[id as usize], true),
        Ty::Vec(_) | Ty::Slice(_) | Ty::Adt(_) | Ty::Func(_) => {
            let index = match ty {
                Ty::Vec(_) | Ty::Slice(_) => 0,
                Ty::Adt(_) => 1,
                _ => 2,
            };
            let addr = load_through(builder, slot, index)?;
            let bank = load_through(builder, slot, index + 1)?;
            adjust_refcount(builder, addr, bank, true)
        }
        _ => Ok(()),
    }
}

fn through_fields(
    builder: &mut IrBuilder,
    proc: &Proc,
    slot: &Value,
    fields: &[Ty],
    retain: bool,
) -> Result<(), CompileError> {
    let mut at = 0i64;
    for field in fields {
        let ptr = builder.build_pointer_offset(slot.clone(), Value::Constant(at), ptr_ty()).ir()?;
        if retain {
            retain_managed_through(builder, proc, &ptr, *field)?;
        } else {
            release_managed_through(builder, proc, &ptr, *field)?;
        }
        at += field.cells();
    }
    Ok(())
}

fn release_managed(builder: &mut IrBuilder, proc: &Proc, frame: &Value, slot: usize) -> Result<(), CompileError> {
    let ty = proc.slots[slot].ty;
    let offset = proc.slots[slot].offset;
    release_tree(builder, proc, frame, offset, ty)
}

fn release_managed_through(builder: &mut IrBuilder, proc: &Proc, slot: &Value, ty: Ty) -> Result<(), CompileError> {
    match ty {
        Ty::Struct { id, .. } => through_fields(builder, proc, slot, &proc.structs[id as usize], false),
        Ty::Tuple { id, .. } => through_fields(builder, proc, slot, &proc.tuples[id as usize], false),
        Ty::Vec(_) | Ty::Slice(_) => {
            let addr = load_through(builder, slot, 0)?;
            let bank = load_through(builder, slot, 1)?;
            adjust_refcount(builder, addr, bank, false)?;
            store_through(builder, slot, 0, Value::Constant(0))?;
            store_through(builder, slot, 1, Value::Constant(0))
        }
        Ty::Adt(id) => {
            let tag = load_through(builder, slot, 0)?;
            let addr = load_through(builder, slot, 1)?;
            let bank = load_through(builder, slot, 2)?;
            call_if_live(builder, &format!("rfl_drop_adt_{id}"), vec![tag, addr, bank])?;
            store_through(builder, slot, 1, Value::Constant(0))?;
            store_through(builder, slot, 2, Value::Constant(0))
        }
        Ty::Func(_) => {
            let id = load_through(builder, slot, 0)?;
            let addr = load_through(builder, slot, 2)?;
            let bank = load_through(builder, slot, 3)?;
            call_if_live(builder, "rfl_drop_func", vec![id, addr, bank])?;
            store_through(builder, slot, 2, Value::Constant(0))?;
            store_through(builder, slot, 3, Value::Constant(0))
        }
        _ => Ok(()),
    }
}

/// `args[1]` is the heap address. A zero address is not an object.
fn call_if_live(builder: &mut IrBuilder, name: &str, args: Vec<Value>) -> Result<(), CompileError> {
    let addr = args[1].clone();
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), skip, go).ir()?;
    builder.create_block(go).ir()?;
    builder.build_call(Value::Global(name.to_string()), args, IrType::Void).ir()?;
    builder.build_branch(skip).ir()?;
    builder.create_block(skip).ir()?;
    Ok(())
}

fn emit_arc(builder: &mut IrBuilder, module: &mut Module, built: &Built) -> Result<(), CompileError> {
    let adt_slots = built.procs.iter().any(|proc| proc.slots.iter().any(|slot| matches!(slot.ty, Ty::Adt(_))));
    let func_slots = built.procs.iter().any(|proc| proc.slots.iter().any(|slot| matches!(slot.ty, Ty::Func(_))));
    let func_fields = built.adts.iter().any(|adt| {
        adt.ctors.iter().any(|ctor| ctor.fields.iter().any(|ty| matches!(ty, Ty::Func(_))))
    });
    let needs_drop_func = func_slots || func_fields || built.procs.iter().any(|proc| !proc.captures.is_empty());
    if adt_slots {
        for (id, adt) in built.adts.iter().enumerate() {
            emit_drop_adt(builder, id as u32, adt, built)?;
            module.add_function(builder.finish_function().unwrap());
        }
    }
    if needs_drop_func {
        emit_drop_func(builder, built)?;
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}

fn emit_drop_adt(builder: &mut IrBuilder, adt_id: u32, adt: &AdtDef, built: &Built) -> Result<(), CompileError> {
    builder.create_function(format!("rfl_drop_adt_{adt_id}"), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    builder.add_parameter(2, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let addr_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let bank_slot = builder.build_alloca(IrType::I16, None).ir()?;
    let tag_slot = builder.build_alloca(IrType::I16, None).ir()?;
    builder.build_store(Value::Temp(0), addr_slot.clone(), IrType::I16).ir()?;
    builder.build_store(Value::Temp(1), bank_slot.clone(), IrType::I16).ir()?;
    builder.build_store(Value::Temp(2), tag_slot.clone(), IrType::I16).ir()?;
    let head = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let addr = Value::Temp(builder.build_load(addr_slot.clone(), IrType::I16).ir()?);
    let is_null = builder.build_binary(IrBinaryOp::Eq, addr.clone(), Value::Constant(0), IrType::I1).ir()?;
    let live = builder.new_label();
    builder.build_branch_cond(Value::Temp(is_null), done, live).ir()?;
    builder.create_block(live).ir()?;
    let bank = Value::Temp(builder.build_load(bank_slot.clone(), IrType::I16).ir()?);
    let tag = Value::Temp(builder.build_load(tag_slot.clone(), IrType::I16).ir()?);
    let obj = fat_from_parts(builder, addr, bank)?;
    let refs = load_cell(builder, &obj, OBJ_REFS, IrType::I16)?;
    let last = builder.build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1).ir()?;
    let drop_it = builder.new_label();
    let dec = builder.new_label();
    builder.build_branch_cond(Value::Temp(last), drop_it, dec).ir()?;
    builder.create_block(dec).ir()?;
    let next = builder.build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16).ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Temp(next))?;
    builder.build_branch(done).ir()?;
    builder.create_block(drop_it).ir()?;
    for (tag_index, ctor) in adt.ctors.iter().enumerate() {
        let matched = builder.new_label();
        let next_check = builder.new_label();
        let is_tag = builder
            .build_binary(IrBinaryOp::Eq, tag.clone(), Value::Constant(tag_index as i64), IrType::I1)
            .ir()?;
        builder.build_branch_cond(Value::Temp(is_tag), matched, next_check).ir()?;
        builder.create_block(matched).ir()?;
        emit_drop_ctor(builder, built, &obj, ctor, adt_id, &addr_slot, &bank_slot, &tag_slot, head, done)?;
        builder.create_block(next_check).ir()?;
    }
    builder.build_call(Value::Global("free".to_string()), vec![obj], IrType::Void).ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    builder.build_return(None).ir()
}

fn emit_drop_ctor(
    builder: &mut IrBuilder,
    built: &Built,
    obj: &Value,
    ctor: &CtorDef,
    adt_id: u32,
    addr_slot: &Value,
    bank_slot: &Value,
    tag_slot: &Value,
    head: u32,
    done: u32,
) -> Result<(), CompileError> {
    let tail_at = ctor.fields.iter().rposition(|field| matches!(field, Ty::Adt(id) if *id == adt_id));
    let mut offset = OBJ_FIELDS;
    let mut tail_tag = None;
    let mut tail_addr = None;
    let mut tail_bank = None;
    for (index, field) in ctor.fields.iter().enumerate() {
        if Some(index) == tail_at {
            tail_tag = Some(load_cell(builder, obj, offset, IrType::I16)?);
            tail_addr = Some(load_cell(builder, obj, offset + 1, IrType::I16)?);
            tail_bank = Some(load_cell(builder, obj, offset + 2, IrType::I16)?);
        } else {
            release_object_field(builder, built, obj, offset, *field)?;
        }
        offset += field.cells();
    }
    builder.build_call(Value::Global("free".to_string()), vec![obj.clone()], IrType::Void).ir()?;
    if let (Some(tag), Some(addr), Some(bank)) = (tail_tag, tail_addr, tail_bank) {
        builder.build_store(addr, addr_slot.clone(), IrType::I16).ir()?;
        builder.build_store(bank, bank_slot.clone(), IrType::I16).ir()?;
        builder.build_store(tag, tag_slot.clone(), IrType::I16).ir()?;
        builder.build_branch(head).ir()
    } else {
        builder.build_branch(done).ir()
    }
}

fn release_object_field(builder: &mut IrBuilder, built: &Built, obj: &Value, offset: i64, ty: Ty) -> Result<(), CompileError> {
    match ty {
        Ty::Struct { id, .. } | Ty::Tuple { id, .. } => {
            let fields = match ty {
                Ty::Struct { id, .. } => built.structs[id as usize].clone(),
                _ => built.tuples[id as usize].clone(),
            };
            let mut at = offset;
            for field in fields {
                release_object_field(builder, built, obj, at, field)?;
                at += field.cells();
            }
            Ok(())
        }
        Ty::Vec(_) | Ty::Slice(_) => {
            let addr = load_cell(builder, obj, offset, IrType::I16)?;
            let bank = load_cell(builder, obj, offset + 1, IrType::I16)?;
            adjust_refcount(builder, addr, bank, false)
        }
        Ty::Adt(id) => {
            let tag = load_cell(builder, obj, offset, IrType::I16)?;
            let addr = load_cell(builder, obj, offset + 1, IrType::I16)?;
            let bank = load_cell(builder, obj, offset + 2, IrType::I16)?;
            call_if_live(builder, &format!("rfl_drop_adt_{id}"), vec![tag, addr, bank])
        }
        Ty::Func(_) => {
            let id = load_cell(builder, obj, offset, IrType::I16)?;
            let addr = load_cell(builder, obj, offset + 2, IrType::I16)?;
            let bank = load_cell(builder, obj, offset + 3, IrType::I16)?;
            call_if_live(builder, "rfl_drop_func", vec![id, addr, bank])
        }
        _ => Ok(()),
    }
}

fn emit_drop_func(builder: &mut IrBuilder, built: &Built) -> Result<(), CompileError> {
    builder.create_function("rfl_drop_func".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    builder.add_parameter(1, IrType::I16);
    builder.add_parameter(2, IrType::I16);
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let skip = builder.new_label();
    let go = builder.new_label();
    let is_null = builder.build_binary(IrBinaryOp::Eq, Value::Temp(1), Value::Constant(0), IrType::I1).ir()?;
    builder.build_branch_cond(Value::Temp(is_null), skip, go).ir()?;
    builder.create_block(go).ir()?;
    let obj = fat_from_parts(builder, Value::Temp(1), Value::Temp(2))?;
    let refs = load_cell(builder, &obj, OBJ_REFS, IrType::I16)?;
    let last = builder.build_binary(IrBinaryOp::Eq, refs.clone(), Value::Constant(1), IrType::I1).ir()?;
    let drop_it = builder.new_label();
    let dec = builder.new_label();
    builder.build_branch_cond(Value::Temp(last), drop_it, dec).ir()?;
    builder.create_block(dec).ir()?;
    let next = builder.build_binary(IrBinaryOp::Sub, refs, Value::Constant(1), IrType::I16).ir()?;
    store_object_cell(builder, &obj, OBJ_REFS, Value::Temp(next))?;
    builder.build_branch(skip).ir()?;
    builder.create_block(drop_it).ir()?;
    let capturing: Vec<&Proc> = built.procs.iter().filter(|proc| !proc.captures.is_empty()).collect();
    for proc in &capturing {
        let matched = builder.new_label();
        let next_check = builder.new_label();
        let is_fn = builder
            .build_binary(IrBinaryOp::Eq, Value::Temp(0), Value::Constant(proc.func_id as i64), IrType::I1)
            .ir()?;
        builder.build_branch_cond(Value::Temp(is_fn), matched, next_check).ir()?;
        builder.create_block(matched).ir()?;
        for cap in &proc.captures {
            release_object_field(builder, built, &obj, cap.offset, cap.ty)?;
        }
        builder.build_call(Value::Global("free".to_string()), vec![obj.clone()], IrType::Void).ir()?;
        builder.build_branch(skip).ir()?;
        builder.create_block(next_check).ir()?;
    }
    builder.build_call(Value::Global("free".to_string()), vec![obj], IrType::Void).ir()?;
    builder.build_branch(skip).ir()?;
    builder.create_block(skip).ir()?;
    builder.build_return(None).ir()
}

fn fat_temp(temp: u32, bank: BankTag) -> Value {
    Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(temp)), bank })
}

fn null_ptr() -> Value {
    Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(0)), bank: BankTag::Null })
}

fn fn_symbol(name: &str) -> String {
    format!("rfl_fn_{name}")
}

fn extern_fn(name: &str, ret: IrType, params: Vec<IrType>) -> Function {
    let mut func = Function::new(name.to_string(), ret);
    func.is_external = true;
    for (index, ty) in params.into_iter().enumerate() {
        func.add_parameter(index as u32, ty);
    }
    func
}

trait IrResult<T> {
    fn ir(self) -> Result<T, CompileError>;
}

impl<T> IrResult<T> for Result<T, String> {
    fn ir(self) -> Result<T, CompileError> {
        self.map_err(CompileError::plain)
    }
}
