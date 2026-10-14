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
use crate::typed::{TBlock, TExpr, TExprKind, TFn, TModule, TStmt};

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
}

#[derive(Clone)]
struct LowerCap {
    /// Binding in the function that creates the closure.
    parent: u32,
    /// Slot in this function that receives the copy.
    slot: usize,
    cells: i64,
    /// Cell offset inside the environment object.
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
}

struct Built {
    procs: Vec<Proc>,
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
        let proc = build_fn(func, func_id)?;
        if proc.segs.iter().any(|seg| matches!(seg.term, Some(Term::Call { .. } | Term::Apply { .. }))) {
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
    Ok(Built { procs, need_alloc, need_fail, need_heap, need_io, need_print_num })
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
}

fn build_fn(func: &TFn, func_id: usize) -> Result<Proc, CompileError> {
    let mut build = FnBuild {
        slots: Vec::new(),
        next_offset: PAYLOAD,
        binding_slot: Vec::new(),
        segs: Vec::new(),
        cur: 0,
        dead: false,
        loops: Vec::new(),
    };
    let result_slot = if func.ret == Ty::Unit { None } else { Some(build.alloc(func.ret)) };
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
    let mut env_offset = 0i64;
    for cap in &func.captures {
        let cells = cap.ty.cells();
        captures.push(LowerCap {
            parent: cap.parent,
            slot: build.binding_slot[cap.local as usize],
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
            if let Some(result) = result {
                self.emit_expr_to(tail, result);
            } else {
                self.emit_expr(tail);
            }
        }
    }

    fn emit_stmt(&mut self, stmt: &TStmt, result: Option<usize>) {
        match stmt {
            TStmt::Let { binding, init } => {
                let dest = self.binding_slot[*binding as usize];
                self.emit_expr_to(init, dest);
            }
            TStmt::Assign { binding, value } => {
                let dest = self.binding_slot[*binding as usize];
                self.emit_expr_to(value, dest);
            }
            TStmt::Expr(expr) => {
                self.emit_expr(expr);
            }
            TStmt::Return(value) => {
                if let (Some(result), Some(value)) = (result, value) {
                    self.emit_expr_to(value, result);
                } else if let Some(value) = value {
                    self.emit_expr(value);
                }
                self.terminate(Term::Return);
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
            TExprKind::Call { callee, args } => {
                let mut arg_slots = Vec::new();
                for arg in args {
                    arg_slots.push(self.emit_expr(arg));
                    if self.dead {
                        return;
                    }
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
                let mut offset = 1i64;
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
        Ty::I16 | Ty::U16 => value as u16 as u64,
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
    }
    if built.need_io {
        module.add_function(extern_fn("putchar", IrType::Void, vec![IrType::I16]));
    }
    if built.need_alloc {
        module.add_function(extern_fn("free", IrType::Void, vec![ptr_ty()]));
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
    if index == 0 && !proc.captures.is_empty() {
        emit_capture_loads(builder, proc, frame)?;
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
    }
}

fn emit_insn(builder: &mut IrBuilder, proc: &Proc, built: &Built, frame: &Value, insn: &Insn) -> Result<(), CompileError> {
    match insn {
        Insn::Const { slot, value } => {
            let slot = &proc.slots[*slot];
            let ptr = gep(builder, frame, slot.offset)?;
            builder.build_store(Value::Constant(*value), ptr, ir_ty(slot.ty)).ir()
        }
        Insn::Copy { dest, src } => copy_slot(builder, proc, frame, *src, proc, frame, *dest),
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
            let base = proc.slots[*dest].offset;
            store_cell(builder, frame, base, Value::Constant(func_id as i64))?;
            store_cell(builder, frame, base + 1, Value::Constant(frame_cells))?;
            if captures.is_empty() {
                store_cell(builder, frame, base + 2, Value::Constant(0))?;
                store_cell(builder, frame, base + 3, Value::Constant(0))
            } else {
                emit_env(builder, proc, frame, base, &captures)
            }
        }
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
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(caller).ir()?;
    let caller_ptr = load_fat(builder, frame, CALLER)?;
    if let Some(result) = proc.result_slot {
        let dest = load_cell(builder, frame, DEST, IrType::I16)?;
        let slot = builder.build_pointer_offset(caller_ptr.clone(), dest, ptr_ty()).ir()?;
        if matches!(proc.ret, Ty::Adt(_) | Ty::Func(_)) {
            let start = proc.slots[result].offset;
            for index in 0..proc.ret.cells() {
                let value = load_cell(builder, frame, start + index, IrType::I16)?;
                let cell = builder.build_pointer_offset(slot.clone(), Value::Constant(index), ptr_ty()).ir()?;
                builder.build_store(value, cell, IrType::I16).ir()?;
            }
        } else {
            let value = load_slot(builder, proc, frame, result)?;
            builder.build_store(value, slot, ir_ty(proc.ret)).ir()?;
        }
    }
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
    if matches!(ty, Ty::Adt(_) | Ty::Func(_)) {
        for index in 0..ty.cells() {
            let value = load_cell(builder, src_frame, src_proc.slots[src].offset + index, IrType::I16)?;
            store_cell(builder, dest_frame, dest_offset + index, value)?;
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
    }
    Ok(())
}

fn emit_env(builder: &mut IrBuilder, proc: &Proc, frame: &Value, base: i64, captures: &[LowerCap]) -> Result<(), CompileError> {
    let words = captures.iter().map(|cap| cap.cells).sum::<i64>();
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
    for cap in captures {
        let src = proc.slots[proc.binding_slots[cap.parent as usize]].offset;
        for index in 0..cap.cells {
            let value = load_cell(builder, frame, src + index, IrType::I16)?;
            store_object_cell(builder, &obj, cap.offset + index, value)?;
        }
    }
    let addr = low16_of_ptr(builder, &obj)?;
    let bank = bank16_of_ptr(builder, &obj)?;
    store_cell(builder, frame, base + 2, addr)?;
    store_cell(builder, frame, base + 3, bank)
}

fn emit_make(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    tag: u16,
    fields: &[(usize, i64)],
) -> Result<(), CompileError> {
    let base = proc.slots[dest].offset;
    store_cell(builder, frame, base, Value::Constant(tag as i64))?;
    if fields.is_empty() {
        store_cell(builder, frame, base + 1, Value::Constant(0))?;
        return store_cell(builder, frame, base + 2, Value::Constant(0));
    }
    let words = 1 + fields.iter().map(|(_, cells)| *cells).sum::<i64>();
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
    store_object_cell(builder, &obj, 0, Value::Constant(tag as i64))?;
    let mut offset = 1i64;
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
    let dest_at = proc.slots[dest].offset;
    for index in 0..cells {
        let ptr = gep(builder, &obj, offset + index)?;
        let value = Value::Temp(builder.build_load(ptr, IrType::I16).ir()?);
        store_cell(builder, frame, dest_at + index, value)?;
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
    if matches!(ty, Ty::Adt(_) | Ty::Func(_)) {
        for index in 0..ty.cells() {
            let value = load_cell(builder, src_frame, src_proc.slots[src].offset + index, IrType::I16)?;
            store_cell(builder, dest_frame, dest_proc.slots[dest].offset + index, value)?;
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
        Ty::I8 | Ty::U8 | Ty::I16 | Ty::U16 => IrType::I16,
        Ty::I32 | Ty::U32 => IrType::I32,
        Ty::I64 | Ty::U64 => IrType::I64,
        Ty::Bool | Ty::Unit | Ty::Adt(_) | Ty::Func(_) => IrType::I16,
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
        Ty::I16 | Ty::U16 => IrType::I16,
        Ty::I32 | Ty::U32 => IrType::I32,
        Ty::I64 | Ty::U64 => IrType::I64,
        Ty::Bool => IrType::I1,
        Ty::Unit => IrType::Void,
        Ty::Adt(_) | Ty::Func(_) => IrType::I16,
    }
}

fn width(ty: Ty) -> u32 {
    match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 | Ty::Bool => 16,
        Ty::I32 | Ty::U32 => 32,
        Ty::I64 | Ty::U64 => 64,
        Ty::Unit | Ty::Adt(_) | Ty::Func(_) => 0,
    }
}

fn ptr_ty() -> IrType {
    IrType::FatPtr(Box::new(IrType::I16))
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
