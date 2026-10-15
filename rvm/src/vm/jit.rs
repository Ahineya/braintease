//! Cranelift JIT. A function is translated the first time it runs.
//!
//! Branches, and the instruction after a call, stay in that function. `JAL`
//! calls the callee's native function and `JALR` returns from it. A compiled
//! call loads the callee from a table and jumps to it; Rust runs only the
//! first time that address is translated, and again after `STORC` clears the
//! slot. The host is entered again when the instruction budget runs out or a
//! store hits the bank-zero header. Guest register writes go to the VM as
//! they happen. `STORC` drops the region that covered the written instruction;
//! that entrance is translated again the next time it runs.
//!
//! `STORC` is the only instruction that writes code memory. The region returns
//! immediately after it, and every region whose instructions cover the written
//! address is dropped. Retired code is reclaimed by rebuilding the module
//! after a few invalidations.
//!
//! IRQs are taken before a region. A region ends at `STORC` and at a store
//! into the bank-zero header, which is where a guest can arm the controller.

use super::instruction::Instr;
use super::VM;
use crate::constants::TEXT40_BASE_WORD;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::{types, InstBuilder};
use cranelift_codegen::ir::{
    AbiParam, Block, BlockArg, FuncRef, MemFlagsData, SigRef, Type, UserFuncName, Value,
};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{FuncId, Linkage, Module};
use ripple_asm::Register;
use std::collections::{HashMap, HashSet};

const CONT: i32 = 0;
const YIELD: i32 = 1;
const FAULT: i32 = 2;

/// One instruction per block, so a resume always lands on an entry that was
/// compiled with this region. Packing more instructions into a block forces
/// Cranelift to rebuild a much larger function whenever execution stops in
/// the middle of it.
const MAX_BLOCK: usize = 1;
/// Branch-connected instructions compiled together, including the code after
/// a call. The callee itself is a separate native function. Past this cap the
/// function returns to the driver, which enters the next slice.
const MAX_BLOCKS: usize = 2048;
const REBUILD_AFTER: u32 = 32;

type JitFn = unsafe extern "C" fn(*mut VM) -> i32;

#[derive(Clone, Copy)]
struct HostIds {
    load: FuncId,
    store: FuncId,
    loadc: FuncId,
    storc: FuncId,
    call: FuncId,
    halt: FuncId,
    brk: FuncId,
    unknown: FuncId,
}

struct Compiled {
    /// Inclusive start and length of each basic block baked into this region.
    ranges: Vec<(usize, usize)>,
    code: JitFn,
}

pub(super) struct JitEngine {
    module: JITModule,
    ids: HostIds,
    blocks: HashMap<usize, Compiled>,
    dead: u32,
    seq: u32,
    /// Native function per guest instruction. Zero means "not compiled".
    /// `table_base` is a stable cell holding the allocation's address, which
    /// compiled calls reload so a later growth of `table` stays visible.
    table: Vec<usize>,
    table_base: Box<usize>,
}

struct Offsets {
    regs: i32,
    executed: i32,
    budget: i32,
}

fn offsets() -> Offsets {
    Offsets {
        regs: i32_offset(std::mem::offset_of!(VM, registers)),
        executed: i32_offset(std::mem::offset_of!(VM, executed_instructions)),
        budget: i32_offset(std::mem::offset_of!(VM, jit_budget)),
    }
}

fn i32_offset(bytes: usize) -> i32 {
    i32::try_from(bytes).unwrap_or_else(|_| panic!("VM field offset {bytes} exceeds i32"))
}

fn is_plain(opcode: u8) -> bool {
    matches!(opcode, 0x01..=0x11 | 0x1A..=0x20 | 0x42)
}

fn branch_cc(opcode: u8) -> IntCC {
    match opcode {
        0x15 => IntCC::Equal,
        0x16 => IntCC::NotEqual,
        0x17 => IntCC::SignedLessThan,
        _ => IntCC::SignedGreaterThanOrEqual,
    }
}

fn no_args() -> &'static [BlockArg] {
    &[]
}

#[derive(Clone, Copy)]
enum Dest {
    Stay(usize),
    Leave,
}

#[derive(Clone, Copy)]
enum Edge {
    Next(Dest),
    Branch { taken: Dest, fall: Dest },
}

struct GuestBlock {
    index: usize,
    instrs: Vec<Instr>,
    edge: Edge,
}

struct Region {
    entry: usize,
    blocks: Vec<GuestBlock>,
}

impl Region {
    fn ranges(&self) -> Vec<(usize, usize)> {
        self.blocks
            .iter()
            .map(|block| (block.index, block.instrs.len()))
            .collect()
    }
}

fn build_region(bank: u16, entry: usize, code: &[Instr]) -> Result<Region, String> {
    if entry >= code.len() {
        return Err(format!("JIT: region entry {entry} is outside code"));
    }
    // Follow branches and the instruction after a call. The callee is a
    // separate native function, entered with a real call.
    let mut leaders = HashSet::new();
    leaders.insert(entry);
    loop {
        let before = leaders.len();
        let current: Vec<usize> = leaders.iter().copied().collect();
        for index in current {
            for succ in decode_at(code, bank, index, &HashSet::new()).successors() {
                if succ < code.len() && leaders.len() < MAX_BLOCKS {
                    leaders.insert(succ);
                }
            }
        }
        if leaders.len() == before {
            break;
        }
    }
    let mut blocks = Vec::new();
    let mut order: Vec<usize> = leaders.iter().copied().collect();
    order.sort_unstable();
    for index in order {
        let mut block = decode_at(code, bank, index, &leaders);
        block.edge = block.edge.resolve(&leaders);
        blocks.push(block);
    }
    Ok(Region { entry, blocks })
}

impl GuestBlock {
    fn successors(&self) -> Vec<usize> {
        self.edge.indices()
    }
}

impl Edge {
    fn indices(&self) -> Vec<usize> {
        match self {
            Edge::Next(dest) => dest.index().into_iter().collect(),
            Edge::Branch { taken, fall } => taken.index().into_iter().chain(fall.index()).collect(),
        }
    }

    fn resolve(self, leaders: &HashSet<usize>) -> Self {
        match self {
            Edge::Next(dest) => Edge::Next(dest.resolve(leaders)),
            Edge::Branch { taken, fall } => Edge::Branch {
                taken: taken.resolve(leaders),
                fall: fall.resolve(leaders),
            },
        }
    }
}

impl Dest {
    fn index(self) -> Option<usize> {
        match self {
            Dest::Stay(index) => Some(index),
            Dest::Leave => None,
        }
    }

    fn resolve(self, leaders: &HashSet<usize>) -> Self {
        match self {
            Dest::Stay(index) if leaders.contains(&index) => Dest::Stay(index),
            _ => Dest::Leave,
        }
    }
}

fn decode_at(code: &[Instr], bank: u16, start: usize, stop: &HashSet<usize>) -> GuestBlock {
    let (mut pc, mut pcb) = locate(bank, start);
    let mut index = start;
    let mut instrs = Vec::new();
    loop {
        if index >= code.len() {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Leave),
            };
        }
        if !instrs.is_empty() && stop.contains(&index) {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Stay(index)),
            };
        }
        let inst = code[index];
        instrs.push(inst);
        if let Some(edge) = control_edge(inst, bank, pc, pcb, code.len()) {
            return GuestBlock {
                index: start,
                instrs,
                edge,
            };
        }
        if writes_pc(&inst) {
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(Dest::Leave),
            };
        }
        if instrs.len() >= MAX_BLOCK {
            let (next_pc, next_pcb) = step_pc(bank, pc, pcb);
            let next = linear(bank, next_pc, next_pcb);
            let dest = if next < code.len() {
                Dest::Stay(next)
            } else {
                Dest::Leave
            };
            return GuestBlock {
                index: start,
                instrs,
                edge: Edge::Next(dest),
            };
        }
        (pc, pcb) = step_pc(bank, pc, pcb);
        index = linear(bank, pc, pcb);
    }
}

fn control_edge(inst: Instr, bank: u16, pc: u16, pcb: u16, len: usize) -> Option<Edge> {
    let inbound = |index: usize| {
        if index < len {
            Dest::Stay(index)
        } else {
            Dest::Leave
        }
    };
    match inst.opcode {
        0x00 | 0x14 | 0x19 | 0x21 => Some(Edge::Next(Dest::Leave)),
        0x12 => {
            let (pc, pcb) = step_pc(bank, pc, pcb);
            Some(Edge::Next(inbound(linear(bank, pc, pcb))))
        }
        // The callee is a native call. The instruction after JAL stays in this
        // function; the link address does not cross a bank boundary.
        0x13 => Some(Edge::Next(inbound(linear(
            bank,
            pc.wrapping_add(1),
            pcb,
        )))),
        0x15..=0x18 => {
            let taken_pc = (pc as i16).wrapping_add(inst.word3 as i16) as u16;
            let (fall_pc, fall_pcb) = step_pc(bank, pc, pcb);
            Some(Edge::Branch {
                taken: inbound(linear(bank, taken_pc, pcb)),
                fall: inbound(linear(bank, fall_pc, fall_pcb)),
            })
        }
        opcode if is_plain(opcode) => None,
        _ => Some(Edge::Next(Dest::Leave)),
    }
}

fn writes_pc(inst: &Instr) -> bool {
    is_plain(inst.opcode) && matches!(inst.word1, 1 | 2)
}

fn locate(bank: u16, index: usize) -> (u16, u16) {
    let bank = usize::from(bank).max(1);
    ((index % bank) as u16, (index / bank) as u16)
}

fn linear(bank: u16, pc: u16, pcb: u16) -> usize {
    usize::from(pcb) * usize::from(bank) + usize::from(pc)
}

fn step_pc(bank: u16, pc: u16, pcb: u16) -> (u16, u16) {
    let next = u32::from(pc) + 1;
    if next >= u32::from(bank) {
        (0, pcb.wrapping_add(1))
    } else {
        (next as u16, pcb)
    }
}

impl JitEngine {
    fn new() -> Result<Self, String> {
        let (module, ids) = build_module()?;
        Ok(Self {
            module,
            ids,
            blocks: HashMap::new(),
            dead: 0,
            seq: 0,
            table: Vec::new(),
            table_base: Box::new(0),
        })
    }

    fn ensure_table(&mut self, len: usize) {
        if len <= self.table.len() {
            return;
        }
        self.table.resize(len, 0);
        *self.table_base = self.table.as_ptr() as usize;
    }

    fn publish(&mut self, index: usize, code: JitFn) {
        self.ensure_table(index + 1);
        self.table[index] = code as usize;
        *self.table_base = self.table.as_ptr() as usize;
    }

    fn clear_slot(&mut self, index: usize) {
        if index < self.table.len() {
            self.table[index] = 0;
        }
    }

    fn get(&self, index: usize) -> Option<JitFn> {
        self.blocks.get(&index).map(|block| block.code)
    }

    fn invalidate(&mut self, idx: usize) {
        let doomed: Vec<usize> = self
            .blocks
            .iter()
            .filter(|(_, block)| {
                block
                    .ranges
                    .iter()
                    .any(|(start, len)| idx >= *start && idx < start + len)
            })
            .map(|(key, _)| *key)
            .collect();
        if doomed.is_empty() {
            return;
        }
        let mut cover = Vec::new();
        for key in &doomed {
            if let Some(block) = self.blocks.get(key) {
                for &(start, len) in &block.ranges {
                    cover.extend(start..start + len);
                }
            }
            cover.push(*key);
        }
        for key in doomed {
            self.blocks.remove(&key);
        }
        for index in cover {
            self.clear_slot(index);
        }
        // One edit invalidates the whole region. Rebuilding the module is the
        // expensive path, and self-modifying stores are rare.
        self.dead = self.dead.saturating_add(1);
    }

    fn sweep(&mut self) -> Result<(), String> {
        if self.dead >= REBUILD_AFTER {
            self.rebuild()?;
        }
        Ok(())
    }

    fn rebuild(&mut self) -> Result<(), String> {
        let (module, ids) = build_module()?;
        self.module = module;
        self.ids = ids;
        self.blocks.clear();
        self.table.fill(0);
        self.dead = 0;
        log::debug!("jit reclaimed invalidated blocks");
        Ok(())
    }

    fn compile(
        &mut self,
        bank_size: u16,
        start: usize,
        region: &Region,
        code_len: usize,
    ) -> Result<JitFn, String> {
        if bank_size == 0 {
            return Err("JIT: bank size is zero".into());
        }
        if region.blocks.is_empty() {
            return Err("JIT: empty region".into());
        }

        let ptr = self.module.target_config().pointer_type();
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(ptr));
        sig.returns.push(AbiParam::new(types::I32));
        self.seq = self.seq.wrapping_add(1);
        let name = format!("ripple_block_{start}_{}", self.seq);
        let id = self
            .module
            .declare_function(&name, Linkage::Export, &sig)
            .map_err(|err| format!("JIT declare failed: {err}"))?;

        let mut ctx = self.module.make_context();
        ctx.func.signature = sig;
        ctx.func.name = UserFuncName::user(0, id.as_u32());
        self.ensure_table(code_len);
        let table_cell = &*self.table_base as *const usize as i64;
        let table_len = self.table.len();
        let guest_sig_data = ctx.func.signature.clone();
        let ids = self.ids;
        let mut func_ctx = FunctionBuilderContext::new();
        let config = self.module.target_config();
        {
            let mut builder = FunctionBuilder::new(&mut ctx.func, &mut func_ctx);
            let entry = builder.create_block();
            builder.append_block_params_for_function_params(entry);
            builder.switch_to_block(entry);
            let vm = builder.block_params(entry)[0];
            let load = self
                .module
                .declare_func_in_func(ids.load, &mut builder.func);
            let store = self
                .module
                .declare_func_in_func(ids.store, &mut builder.func);
            let loadc = self
                .module
                .declare_func_in_func(ids.loadc, &mut builder.func);
            let storc = self
                .module
                .declare_func_in_func(ids.storc, &mut builder.func);
            let call = self
                .module
                .declare_func_in_func(ids.call, &mut builder.func);
            let halt = self
                .module
                .declare_func_in_func(ids.halt, &mut builder.func);
            let brk = self.module.declare_func_in_func(ids.brk, &mut builder.func);
            let unknown = self
                .module
                .declare_func_in_func(ids.unknown, &mut builder.func);
            let guest_sig = builder.func.import_signature(guest_sig_data);
            let mut var_reg = [Variable::from_u32(0); 32];
            for index in 0..32 {
                var_reg[index] = builder.declare_var(types::I16);
            }
            let var_executed = builder.declare_var(types::I64);
            let var_budget = builder.declare_var(types::I64);
            let mut cg = Cg {
                b: builder,
                vm,
                var_reg,
                var_executed,
                var_budget,
                yield_exit: None,
                fault_exit: None,
                cont_exit: None,
                resume: None,
                bank_size,
                flags: MemFlagsData::trusted(),
                off: offsets(),
                load,
                store,
                loadc,
                storc,
                call,
                halt,
                brk,
                unknown,
                ptr,
                guest_sig,
                table_cell,
                table_len,
            };
            cg.emit_region(region)?;
            cg.b.seal_all_blocks();
            cg.b.finalize(config);
        }
        if let Err(err) = self.module.define_function(id, &mut ctx) {
            return Err(format!("JIT compile failed: {err}\n{}", ctx.func));
        }
        self.module.clear_context(&mut ctx);
        self.module
            .finalize_definitions()
            .map_err(|err| format!("JIT finalize failed: {err}"))?;
        let ptr = self.module.get_finalized_function(id);
        let code: JitFn = unsafe { std::mem::transmute(ptr) };
        log::debug!(
            "jit compiled region {start} ({} blocks)",
            region.blocks.len()
        );
        let ranges = region.ranges();
        for block in &region.blocks {
            self.blocks.insert(
                block.index,
                Compiled {
                    ranges: ranges.clone(),
                    code,
                },
            );
            self.publish(block.index, code);
        }
        Ok(code)
    }
}

struct Cg<'a> {
    b: FunctionBuilder<'a>,
    vm: Value,
    var_reg: [Variable; 32],
    var_executed: Variable,
    var_budget: Variable,
    yield_exit: Option<Block>,
    fault_exit: Option<Block>,
    cont_exit: Option<Block>,
    /// Dispatch block. A call returns here when the guest PC is not the
    /// instruction immediately after the call.
    resume: Option<Block>,
    bank_size: u16,
    flags: MemFlagsData,
    off: Offsets,
    load: FuncRef,
    store: FuncRef,
    loadc: FuncRef,
    storc: FuncRef,
    call: FuncRef,
    halt: FuncRef,
    brk: FuncRef,
    unknown: FuncRef,
    ptr: Type,
    guest_sig: SigRef,
    /// Address of the stable cell that holds the call-table pointer.
    table_cell: i64,
    table_len: usize,
}

impl<'a> Cg<'a> {
    fn emit_region(&mut self, region: &Region) -> Result<(), String> {
        self.preload();
        let mut blocks = HashMap::new();
        for block in &region.blocks {
            blocks.insert(block.index, self.b.create_block());
        }
        let resume = self.b.create_block();
        self.resume = Some(resume);
        self.b.ins().jump(resume, no_args());
        self.b.switch_to_block(resume);
        self.dispatch(region, &blocks);
        for block in &region.blocks {
            self.emit_guest(block, &blocks)?;
        }
        self.emit_exits();
        Ok(())
    }

    /// Every return from this function spills once. A private spill on each
    /// instruction makes Cranelift's allocator slower than the guest.
    fn emit_exits(&mut self) {
        let exits = [
            (self.yield_exit, YIELD),
            (self.fault_exit, FAULT),
            (self.cont_exit, CONT),
        ];
        for (block, status) in exits {
            let Some(block) = block else {
                continue;
            };
            self.b.switch_to_block(block);
            self.spill();
            let status = self.k32(i64::from(status));
            self.b.ins().return_(&[status]);
        }
    }

    /// The same native function is entered at every block in the region.
    fn dispatch(&mut self, region: &Region, blocks: &HashMap<usize, Block>) {
        let here = self.guest_index();
        let mut order = Vec::with_capacity(region.blocks.len());
        order.push(region.entry);
        for block in &region.blocks {
            if block.index != region.entry {
                order.push(block.index);
            }
        }
        for (nth, guest) in order.iter().copied().enumerate() {
            let found = blocks[&guest];
            let want = self.k32(guest as i64);
            let same = self.b.ins().icmp(IntCC::Equal, here, want);
            let miss = self.b.create_block();
            self.b.ins().brif(same, found, no_args(), miss, no_args());
            self.b.switch_to_block(miss);
            if nth + 1 == order.len() {
                // The guest PC belongs to some other function, usually because
                // a callee returned somewhere other than the normal link site.
                // The dispatcher looks the address up again.
                self.finish(CONT);
            }
        }
    }

    fn guest_index(&mut self) -> Value {
        let pc = self.reg(Register::Pc as usize);
        let pcb = self.reg(Register::Pcb as usize);
        let pc = self.b.ins().uextend(types::I32, pc);
        let pcb = self.b.ins().uextend(types::I32, pcb);
        let bank = self.k32(i64::from(self.bank_size));
        let scaled = self.b.ins().imul(pcb, bank);
        self.b.ins().iadd(scaled, pc)
    }

    fn preload(&mut self) {
        let zero = self.k16(0);
        self.b.def_var(self.var_reg[0], zero);
        for index in 1..32 {
            let value = self.load16(self.reg_off(index));
            self.b.def_var(self.var_reg[index], value);
        }
        let executed = self.load64(self.off.executed);
        let budget = self.load64(self.off.budget);
        self.b.def_var(self.var_executed, executed);
        self.b.def_var(self.var_budget, budget);
    }

    fn emit_guest(
        &mut self,
        block: &GuestBlock,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
        let Some(cl) = blocks.get(&block.index).copied() else {
            return Err(format!("JIT: missing cranelift block {}", block.index));
        };
        self.b.switch_to_block(cl);
        let last = block.instrs.len() - 1;
        for inst in &block.instrs[..last] {
            self.emit_inner(inst);
        }
        let inst = &block.instrs[last];
        match inst.opcode {
            0x15..=0x18 => {
                let Edge::Branch { taken, fall } = block.edge else {
                    return Err("JIT: branch is missing its edges".into());
                };
                self.emit_branch_edge(inst, taken, fall, blocks)
            }
            0x00 => {
                self.emit_halt();
                Ok(())
            }
            0x19 => {
                self.emit_brk();
                Ok(())
            }
            0x14 => {
                self.emit_jalr(inst);
                Ok(())
            }
            0x21 => {
                self.emit_storc_end(inst);
                Ok(())
            }
            0x13 => {
                self.emit_jal_effect(inst);
                let Edge::Next(dest) = block.edge else {
                    return Err("JIT: jal is missing its destination".into());
                };
                self.emit_native_call(inst, dest, blocks)
            }
            0x12 => {
                self.emit_store_effect(inst);
                let Edge::Next(dest) = block.edge else {
                    return Err("JIT: store is missing its destination".into());
                };
                self.take(dest, blocks)
            }
            opcode if !is_plain(opcode) => {
                self.emit_unknown(opcode);
                Ok(())
            }
            _ => {
                self.emit_inner(inst);
                let Edge::Next(dest) = block.edge else {
                    return Err("JIT: straight block has a branch edge".into());
                };
                self.take(dest, blocks)
            }
        }
    }

    fn take(&mut self, dest: Dest, blocks: &HashMap<usize, Block>) -> Result<(), String> {
        match dest {
            Dest::Leave => {
                self.finish(CONT);
                Ok(())
            }
            Dest::Stay(index) => {
                let Some(block) = blocks.get(&index).copied() else {
                    return Err(format!("JIT: successor {index} was not compiled"));
                };
                self.b.ins().jump(block, no_args());
                Ok(())
            }
        }
    }

    /// Returns true when every path through this instruction already returned.
    fn emit(&mut self, inst: &Instr) -> bool {
        self.ensure_budget();
        match inst.opcode {
            0x00 => {
                self.b.ins().call(self.halt, &[self.vm]);
                self.bump();
                self.advance();
                self.finish(YIELD);
                true
            }
            0x42 => {
                self.counted();
                false
            }
            0x01 => self.rr(inst, |cg, rs, rt| cg.b.ins().iadd(rs, rt)),
            0x02 => self.rr(inst, |cg, rs, rt| cg.b.ins().isub(rs, rt)),
            0x03 => self.rr(inst, |cg, rs, rt| cg.b.ins().band(rs, rt)),
            0x04 => self.rr(inst, |cg, rs, rt| cg.b.ins().bor(rs, rt)),
            0x05 => self.rr(inst, |cg, rs, rt| cg.b.ins().bxor(rs, rt)),
            0x06 => self.rr(inst, |cg, rs, rt| {
                let amount = cg.masked_shift(rt);
                cg.b.ins().ishl(rs, amount)
            }),
            0x07 => self.rr(inst, |cg, rs, rt| {
                let amount = cg.masked_shift(rt);
                cg.b.ins().ushr(rs, amount)
            }),
            0x08 => self.rr(inst, |cg, rs, rt| cg.flag(IntCC::SignedLessThan, rs, rt)),
            0x09 => self.rr(inst, |cg, rs, rt| cg.flag(IntCC::UnsignedLessThan, rs, rt)),
            0x0A => self.ri(inst, |cg, rs, imm| {
                let imm = cg.k16(imm);
                cg.b.ins().iadd(rs, imm)
            }),
            0x0B => self.ri(inst, |cg, rs, imm| {
                let imm = cg.k16(imm);
                cg.b.ins().band(rs, imm)
            }),
            0x0C => self.ri(inst, |cg, rs, imm| {
                let imm = cg.k16(imm);
                cg.b.ins().bor(rs, imm)
            }),
            0x0D => self.ri(inst, |cg, rs, imm| {
                let imm = cg.k16(imm);
                cg.b.ins().bxor(rs, imm)
            }),
            0x0E => {
                if let Some(rd) = rx(inst.word1) {
                    let value = self.k16(inst.word2);
                    self.set_reg(rd, value);
                }
                self.counted();
                false
            }
            0x0F => self.ri(inst, |cg, rs, imm| {
                cg.b.ins().ishl_imm_u(rs, i64::from(imm & 15))
            }),
            0x10 => self.ri(inst, |cg, rs, imm| {
                cg.b.ins().ushr_imm_u(rs, i64::from(imm & 15))
            }),
            0x11 => self.mem_load(inst),
            0x12 => self.mem_store(inst),
            0x13 => self.jal(inst),
            0x14 => self.jalr(inst),
            0x15 => self.branch(inst, IntCC::Equal),
            0x16 => self.branch(inst, IntCC::NotEqual),
            0x17 => self.branch(inst, IntCC::SignedLessThan),
            0x18 => self.branch(inst, IntCC::SignedGreaterThanOrEqual),
            0x19 => {
                self.spill();
                self.b.ins().call(self.brk, &[self.vm]);
                self.bump();
                self.advance();
                self.finish(YIELD);
                true
            }
            0x1A => self.rr(inst, |cg, rs, rt| cg.b.ins().imul(rs, rt)),
            0x1B => self.rr(inst, |cg, rs, rt| cg.divrem(rs, rt, false)),
            0x1C => self.rr(inst, |cg, rs, rt| cg.divrem(rs, rt, true)),
            0x1D => self.ri(inst, |cg, rs, imm| {
                let imm = cg.k16(imm);
                cg.b.ins().imul(rs, imm)
            }),
            0x1E => self.ri(inst, |cg, rs, imm| cg.divrem_imm(rs, imm, false)),
            0x1F => self.ri(inst, |cg, rs, imm| cg.divrem_imm(rs, imm, true)),
            0x20 => self.loadc(inst),
            0x21 => self.storc(inst),
            opcode => {
                let code = self.k32(i64::from(opcode));
                self.b.ins().call(self.unknown, &[self.vm, code]);
                self.finish(FAULT);
                true
            }
        }
    }

    fn rr(&mut self, inst: &Instr, op: impl FnOnce(&mut Self, Value, Value) -> Value) -> bool {
        if let (Some(rd), Some(rs), Some(rt)) = (rx(inst.word1), rx(inst.word2), rx(inst.word3)) {
            let rs = self.reg(rs);
            let rt = self.reg(rt);
            let value = op(self, rs, rt);
            self.set_reg(rd, value);
        }
        self.counted();
        false
    }

    fn ri(&mut self, inst: &Instr, op: impl FnOnce(&mut Self, Value, u16) -> Value) -> bool {
        if let (Some(rd), Some(rs)) = (rx(inst.word1), rx(inst.word2)) {
            let rs = self.reg(rs);
            let value = op(self, rs, inst.word3);
            self.set_reg(rd, value);
        }
        self.counted();
        false
    }

    fn mem_load(&mut self, inst: &Instr) -> bool {
        let Some(rd) = rx(inst.word1) else {
            self.counted();
            return false;
        };
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            return false;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            return false;
        };
        let bank = self.reg(bank);
        let addr = self.reg(addr);
        let bank = self.zext(bank);
        let addr = self.zext(addr);
        let raw = self.call_i32(self.load, &[self.vm, bank, addr]);
        let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
        self.side_if(fault, FAULT);
        let value = self.b.ins().ireduce(types::I16, raw);
        self.set_reg(rd, value);
        self.counted();
        false
    }

    fn mem_store(&mut self, inst: &Instr) -> bool {
        if let (Some(src), Some(bank), Some(addr)) =
            (rx(inst.word1), rx(inst.word2), rx(inst.word3))
        {
            let value = self.reg(src);
            let bank = self.reg(bank);
            let addr = self.reg(addr);
            let value = self.zext(value);
            let bank = self.zext(bank);
            let addr = self.zext(addr);
            let raw = self.call_i32(self.store, &[self.vm, bank, addr, value]);
            let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
            self.side_if(fault, FAULT);
            self.bump();
            self.advance();
            let boundary = self.b.ins().icmp_imm_s(IntCC::Equal, raw, 1);
            self.side_if(boundary, YIELD);
        } else {
            self.counted();
        }
        self.finish(CONT);
        true
    }

    fn jal(&mut self, inst: &Instr) -> bool {
        let pc = self.reg(Register::Pc as usize);
        let ret_pc = self.wrapping_add1(pc);
        let ret_pcb = self.reg(Register::Pcb as usize);
        if let Some(rd) = rx(inst.word1).filter(|rd| *rd != Register::R0 as usize) {
            self.set_reg(rd, ret_pc);
            self.set_reg(Register::Rab as usize, ret_pcb);
        }
        let bank = self.k16(inst.word2);
        let addr = self.k16(inst.word3);
        self.set_reg(Register::Pcb as usize, bank);
        self.set_reg(Register::Pc as usize, addr);
        self.bump();
        self.finish(CONT);
        true
    }

    fn jalr(&mut self, inst: &Instr) -> bool {
        let Some(rd) = rx(inst.word1) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let new_pc = self.reg(addr);
        let new_pcb = self.reg(bank);
        let pc = self.reg(Register::Pc as usize);
        let ret_pc = self.wrapping_add1(pc);
        let ret_pcb = self.reg(Register::Pcb as usize);
        if rd != Register::R0 as usize {
            self.set_reg(rd, ret_pc);
            self.set_reg(Register::Rab as usize, ret_pcb);
        }
        self.set_reg(Register::Pcb as usize, new_pcb);
        self.set_reg(Register::Pc as usize, new_pc);
        self.bump();
        self.finish(CONT);
        true
    }

    fn branch(&mut self, inst: &Instr, cc: IntCC) -> bool {
        let Some(rs) = rx(inst.word1) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let Some(rt) = rx(inst.word2) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let rs = self.reg(rs);
        let rt = self.reg(rt);
        let taken = self.b.ins().icmp(cc, rs, rt);
        let pc = self.reg(Register::Pc as usize);
        let pcb = self.reg(Register::Pcb as usize);
        let (fall_pc, fall_pcb) = self.advanced(pc, pcb);
        let offset = self.k16_signed(inst.word3 as i16);
        let dest = self.b.ins().iadd(pc, offset);
        let new_pc = self.b.ins().select(taken, dest, fall_pc);
        let new_pcb = self.b.ins().select(taken, pcb, fall_pcb);
        self.set_reg(Register::Pc as usize, new_pc);
        self.set_reg(Register::Pcb as usize, new_pcb);
        self.bump();
        self.finish(CONT);
        true
    }

    fn loadc(&mut self, inst: &Instr) -> bool {
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            return false;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            return false;
        };
        let bank = self.reg(bank);
        let addr = self.reg(addr);
        let bank = self.zext(bank);
        let addr = self.zext(addr);
        let raw = self.call_i32(self.loadc, &[self.vm, bank, addr]);
        let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
        self.side_if(fault, FAULT);
        for reg in Register::X0 as usize..=Register::X3 as usize {
            self.reload(reg);
        }
        self.counted();
        false
    }

    fn storc(&mut self, inst: &Instr) -> bool {
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            self.finish(CONT);
            return true;
        };
        let bank = self.reg(bank);
        let addr = self.reg(addr);
        let bank = self.zext(bank);
        let addr = self.zext(addr);
        self.spill();
        let raw = self.call_i32(self.storc, &[self.vm, bank, addr]);
        let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
        self.side_if(fault, FAULT);
        self.bump();
        self.advance();
        self.finish(CONT);
        true
    }

    fn flag(&mut self, cc: IntCC, left: Value, right: Value) -> Value {
        let bit = self.b.ins().icmp(cc, left, right);
        self.b.ins().uextend(types::I16, bit)
    }

    fn masked_shift(&mut self, amount: Value) -> Value {
        let mask = self.k16(15);
        self.b.ins().band(amount, mask)
    }

    fn divrem(&mut self, left: Value, right: Value, rem: bool) -> Value {
        let numer = self.b.ins().uextend(types::I32, left);
        let denom = self.b.ins().uextend(types::I32, right);
        let zero = self.k32(0);
        let one = self.k32(1);
        let is_zero = self.b.ins().icmp_imm_s(IntCC::Equal, denom, 0);
        let safe = self.b.ins().select(is_zero, one, denom);
        let raw = if rem {
            self.b.ins().urem(numer, safe)
        } else {
            self.b.ins().udiv(numer, safe)
        };
        let raw = self.b.ins().select(is_zero, zero, raw);
        self.b.ins().ireduce(types::I16, raw)
    }

    fn divrem_imm(&mut self, left: Value, imm: u16, rem: bool) -> Value {
        if imm == 0 {
            return self.k16(0);
        }
        let numer = self.b.ins().uextend(types::I32, left);
        let denom = self.k32(i64::from(imm));
        let raw = if rem {
            self.b.ins().urem(numer, denom)
        } else {
            self.b.ins().udiv(numer, denom)
        };
        self.b.ins().ireduce(types::I16, raw)
    }

    fn emit_branch_edge(
        &mut self,
        inst: &Instr,
        taken: Dest,
        fall: Dest,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
        self.ensure_budget();
        let (cond, dest_pc, dest_pcb, fall_pc, fall_pcb) =
            if let (Some(rs), Some(rt)) = (rx(inst.word1), rx(inst.word2)) {
                let rs = self.reg(rs);
                let rt = self.reg(rt);
                let cc = branch_cc(inst.opcode);
                let cond = self.b.ins().icmp(cc, rs, rt);
                let pc = self.reg(Register::Pc as usize);
                let pcb = self.reg(Register::Pcb as usize);
                let (fall_pc, fall_pcb) = self.advanced(pc, pcb);
                let offset = self.k16_signed(inst.word3 as i16);
                let dest_pc = self.b.ins().iadd(pc, offset);
                (cond, dest_pc, pcb, fall_pc, fall_pcb)
            } else {
                self.counted();
                return self.take(fall, blocks);
            };
        self.bump();
        let go = self.b.create_block();
        let stay = self.b.create_block();
        self.b.ins().brif(cond, go, no_args(), stay, no_args());
        self.b.switch_to_block(go);
        self.set_reg(Register::Pc as usize, dest_pc);
        self.set_reg(Register::Pcb as usize, dest_pcb);
        self.take(taken, blocks)?;
        self.b.switch_to_block(stay);
        self.set_reg(Register::Pc as usize, fall_pc);
        self.set_reg(Register::Pcb as usize, fall_pcb);
        self.take(fall, blocks)
    }

    fn emit_halt(&mut self) {
        self.ensure_budget();
        self.b.ins().call(self.halt, &[self.vm]);
        self.bump();
        self.advance();
        self.finish(YIELD);
    }

    fn emit_brk(&mut self) {
        self.ensure_budget();
        self.spill();
        self.b.ins().call(self.brk, &[self.vm]);
        self.bump();
        self.advance();
        self.finish(YIELD);
    }

    fn emit_jal_effect(&mut self, inst: &Instr) {
        self.ensure_budget();
        let pc = self.reg(Register::Pc as usize);
        let ret_pc = self.wrapping_add1(pc);
        let ret_pcb = self.reg(Register::Pcb as usize);
        if let Some(rd) = rx(inst.word1).filter(|rd| *rd != Register::R0 as usize) {
            self.set_reg(rd, ret_pc);
            self.set_reg(Register::Rab as usize, ret_pcb);
        }
        let bank = self.k16(inst.word2);
        let addr = self.k16(inst.word3);
        self.set_reg(Register::Pcb as usize, bank);
        self.set_reg(Register::Pc as usize, addr);
        self.bump();
    }

    /// Call a compiled callee directly when its slot is filled. The first call
    /// and any call after `STORC` clears the slot go through Rust, which
    /// compiles and publishes the slot.
    fn call_guest(&mut self, target: usize) -> Result<Value, String> {
        let target_i32 = i32::try_from(target)
            .map_err(|_| format!("JIT call target {target} does not fit in i32"))?;
        if target >= self.table_len {
            let index = self.k32(i64::from(target_i32));
            return Ok(self.call_i32(self.call, &[self.vm, index]));
        }
        let cell = self.b.ins().iconst(self.ptr, self.table_cell);
        let base = self.b.ins().load(self.ptr, self.flags, cell, 0);
        let no_table = self.b.ins().icmp_imm_s(IntCC::Equal, base, 0);
        let have = self.b.create_block();
        let slow = self.b.create_block();
        let join = self.b.create_block();
        self.b.append_block_param(join, types::I32);
        self.b.ins().brif(no_table, slow, no_args(), have, no_args());

        self.b.switch_to_block(have);
        let slot = self
            .b
            .ins()
            .iadd_imm_s(base, i64::from(target_i32) * 8);
        let fp = self.b.ins().load(self.ptr, self.flags, slot, 0);
        let missing = self.b.ins().icmp_imm_s(IntCC::Equal, fp, 0);
        let hit = self.b.create_block();
        self.b.ins().brif(missing, slow, no_args(), hit, no_args());

        self.b.switch_to_block(hit);
        let direct = self
            .b
            .ins()
            .call_indirect(self.guest_sig, fp, &[self.vm]);
        let direct = self.b.inst_results(direct)[0];
        let direct_arg = BlockArg::Value(direct);
        self.b.ins().jump(join, std::slice::from_ref(&direct_arg));

        self.b.switch_to_block(slow);
        let index = self.k32(i64::from(target_i32));
        let slow_status = self.call_i32(self.call, &[self.vm, index]);
        let slow_arg = BlockArg::Value(slow_status);
        self.b.ins().jump(join, std::slice::from_ref(&slow_arg));

        self.b.switch_to_block(join);
        Ok(self.b.block_params(join)[0])
    }

    /// Publish registers, call the callee, and resume unless it yielded or
    /// faulted. The callee has its own variables and reads the VM.
    fn emit_native_call(
        &mut self,
        inst: &Instr,
        dest: Dest,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
        let budget = self.b.use_var(self.var_budget);
        let empty = self.b.ins().icmp_imm_s(IntCC::Equal, budget, 0);
        self.side_if(empty, YIELD);

        self.spill();
        let target = linear(self.bank_size, inst.word3, inst.word2);
        let status = self.call_guest(target)?;
        let stop = self.b.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
        let cont = self.b.create_block();
        let side = self.b.create_block();
        self.b
            .ins()
            .brif(stop, side, no_args(), cont, no_args());
        self.b.switch_to_block(side);
        self.b.ins().return_(&[status]);
        self.b.switch_to_block(cont);
        self.reload_all();
        self.resume_at(dest, blocks)
    }

    fn resume_at(
        &mut self,
        dest: Dest,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
        let Some(resume) = self.resume else {
            return Err("JIT: call has no resume block".into());
        };
        let Dest::Stay(index) = dest else {
            self.b.ins().jump(resume, no_args());
            return Ok(());
        };
        let Some(block) = blocks.get(&index).copied() else {
            return Err(format!(
                "JIT: call returns to {index}, which was not compiled"
            ));
        };
        let here = self.guest_index();
        let want = self.k32(index as i64);
        let same = self.b.ins().icmp(IntCC::Equal, here, want);
        let other = self.b.create_block();
        self.b
            .ins()
            .brif(same, block, no_args(), other, no_args());
        self.b.switch_to_block(other);
        self.b.ins().jump(resume, no_args());
        Ok(())
    }

    fn reload_all(&mut self) {
        let zero = self.k16(0);
        self.b.def_var(self.var_reg[0], zero);
        for index in 1..32 {
            self.reload(index);
        }
        let executed = self.load64(self.off.executed);
        let budget = self.load64(self.off.budget);
        self.b.def_var(self.var_executed, executed);
        self.b.def_var(self.var_budget, budget);
    }

    fn emit_jalr(&mut self, inst: &Instr) {
        self.ensure_budget();
        let Some(rd) = rx(inst.word1) else {
            self.counted();
            self.finish(CONT);
            return;
        };
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            self.finish(CONT);
            return;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            self.finish(CONT);
            return;
        };
        let new_pc = self.reg(addr);
        let new_pcb = self.reg(bank);
        let pc = self.reg(Register::Pc as usize);
        let ret_pc = self.wrapping_add1(pc);
        let ret_pcb = self.reg(Register::Pcb as usize);
        if rd != Register::R0 as usize {
            self.set_reg(rd, ret_pc);
            self.set_reg(Register::Rab as usize, ret_pcb);
        }
        self.set_reg(Register::Pcb as usize, new_pcb);
        self.set_reg(Register::Pc as usize, new_pc);
        self.bump();
        self.finish(CONT);
    }

    fn emit_store_effect(&mut self, inst: &Instr) {
        self.ensure_budget();
        if let (Some(src), Some(bank), Some(addr)) =
            (rx(inst.word1), rx(inst.word2), rx(inst.word3))
        {
            let value = self.reg(src);
            let bank = self.reg(bank);
            let addr = self.reg(addr);
            let value = self.zext(value);
            let bank = self.zext(bank);
            let addr = self.zext(addr);
            let raw = self.call_i32(self.store, &[self.vm, bank, addr, value]);
            let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
            self.side_if(fault, FAULT);
            self.bump();
            self.advance();
            let boundary = self.b.ins().icmp_imm_s(IntCC::Equal, raw, 1);
            self.side_if(boundary, YIELD);
        } else {
            self.counted();
        }
    }

    fn emit_storc_end(&mut self, inst: &Instr) {
        self.ensure_budget();
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            self.finish(CONT);
            return;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            self.finish(CONT);
            return;
        };
        let bank = self.reg(bank);
        let addr = self.reg(addr);
        let bank = self.zext(bank);
        let addr = self.zext(addr);
        self.spill();
        let raw = self.call_i32(self.storc, &[self.vm, bank, addr]);
        let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
        self.side_if(fault, FAULT);
        self.bump();
        self.advance();
        self.finish(CONT);
    }

    fn emit_unknown(&mut self, opcode: u8) {
        self.ensure_budget();
        let code = self.k32(i64::from(opcode));
        self.b.ins().call(self.unknown, &[self.vm, code]);
        self.finish(FAULT);
    }

    fn emit_inner(&mut self, inst: &Instr) {
        let _ = self.emit(inst);
    }

    fn counted(&mut self) {
        self.bump();
        self.advance();
    }

    fn ensure_budget(&mut self) {
        let budget = self.b.use_var(self.var_budget);
        let empty = self.b.ins().icmp_imm_s(IntCC::Equal, budget, 0);
        self.side_if(empty, YIELD);
    }

    fn side_if(&mut self, cond: Value, status: i32) {
        let cont = self.b.create_block();
        let side = self.b.create_block();
        self.b.ins().brif(cond, side, no_args(), cont, no_args());
        self.b.switch_to_block(side);
        self.finish(status);
        self.b.switch_to_block(cont);
    }

    fn advance(&mut self) {
        let pc = self.reg(Register::Pc as usize);
        let pcb = self.reg(Register::Pcb as usize);
        let (pc, pcb) = self.advanced(pc, pcb);
        self.set_reg(Register::Pc as usize, pc);
        self.set_reg(Register::Pcb as usize, pcb);
    }

    fn advanced(&mut self, pc: Value, pcb: Value) -> (Value, Value) {
        let pc32 = self.b.ins().uextend(types::I32, pc);
        let next = self.b.ins().iadd_imm_s(pc32, 1);
        let limit = self.k32(i64::from(self.bank_size));
        let wrap = self
            .b
            .ins()
            .icmp(IntCC::UnsignedGreaterThanOrEqual, next, limit);
        let linear = self.b.ins().ireduce(types::I16, next);
        let zero = self.k16(0);
        let new_pc = self.b.ins().select(wrap, zero, linear);
        let bumped = self.b.ins().iadd_imm_s(pcb, 1);
        let new_pcb = self.b.ins().select(wrap, bumped, pcb);
        (new_pc, new_pcb)
    }

    fn wrapping_add1(&mut self, value: Value) -> Value {
        let one = self.k16(1);
        self.b.ins().iadd(value, one)
    }

    fn bump(&mut self) {
        let executed = self.b.use_var(self.var_executed);
        let executed = self.b.ins().iadd_imm_s(executed, 1);
        self.b.def_var(self.var_executed, executed);
        let budget = self.b.use_var(self.var_budget);
        let budget = self.b.ins().iadd_imm_s(budget, -1);
        self.b.def_var(self.var_budget, budget);
    }

    fn finish(&mut self, status: i32) {
        let exit = self.exit_for(status);
        self.b.ins().jump(exit, no_args());
    }

    fn exit_for(&mut self, status: i32) -> Block {
        let slot = match status {
            YIELD => &mut self.yield_exit,
            FAULT => &mut self.fault_exit,
            _ => &mut self.cont_exit,
        };
        if let Some(block) = *slot {
            return block;
        }
        let block = self.b.create_block();
        *slot = Some(block);
        block
    }

    fn spill(&mut self) {
        // Register writes are already in the VM. Only the counters are cached.
        let executed = self.b.use_var(self.var_executed);
        let budget = self.b.use_var(self.var_budget);
        self.store64(self.off.executed, executed);
        self.store64(self.off.budget, budget);
    }

    fn reg(&mut self, index: usize) -> Value {
        if index == 0 || index >= 32 {
            return self.k16(0);
        }
        self.b.use_var(self.var_reg[index])
    }

    fn set_reg(&mut self, index: usize, value: Value) {
        if index == 0 || index >= 32 {
            return;
        }
        self.b.def_var(self.var_reg[index], value);
        self.store16(self.reg_off(index), value);
    }

    fn reload(&mut self, index: usize) {
        if index == 0 || index >= 32 {
            return;
        }
        let value = self.load16(self.reg_off(index));
        self.b.def_var(self.var_reg[index], value);
    }

    fn call_i32(&mut self, func: FuncRef, args: &[Value]) -> Value {
        let inst = self.b.ins().call(func, args);
        self.b.inst_results(inst)[0]
    }

    fn zext(&mut self, value: Value) -> Value {
        self.b.ins().uextend(types::I32, value)
    }

    fn k16(&mut self, imm: u16) -> Value {
        self.b.ins().iconst(types::I16, i64::from(imm))
    }

    fn k16_signed(&mut self, imm: i16) -> Value {
        self.b.ins().iconst(types::I16, i64::from(imm))
    }

    fn k32(&mut self, imm: i64) -> Value {
        self.b.ins().iconst(types::I32, imm)
    }

    fn reg_off(&self, index: usize) -> i32 {
        self.off.regs + i32::try_from(index * 2).unwrap_or(0)
    }

    fn load16(&mut self, offset: i32) -> Value {
        self.b.ins().load(types::I16, self.flags, self.vm, offset)
    }

    fn load64(&mut self, offset: i32) -> Value {
        self.b.ins().load(types::I64, self.flags, self.vm, offset)
    }

    fn store16(&mut self, offset: i32, value: Value) {
        self.b.ins().store(self.flags, value, self.vm, offset);
    }

    fn store64(&mut self, offset: i32, value: Value) {
        self.b.ins().store(self.flags, value, self.vm, offset);
    }
}

fn rx(word: u16) -> Option<usize> {
    let index = usize::from(word);
    (index < 32).then_some(index)
}

fn build_module() -> Result<(JITModule, HostIds), String> {
    let mut builder = JITBuilder::with_flags(
        // `speed` made this guest slower overall: the extra register allocation
        // cost more than the tighter code saved.
        &[("opt_level", "none")],
        cranelift_module::default_libcall_names(),
    )
    .map_err(|err| format!("JIT: {err}"))?;
    builder.symbol("rvm_jit_load", rvm_jit_load as *const u8);
    builder.symbol("rvm_jit_store", rvm_jit_store as *const u8);
    builder.symbol("rvm_jit_loadc", rvm_jit_loadc as *const u8);
    builder.symbol("rvm_jit_storc", rvm_jit_storc as *const u8);
    builder.symbol("rvm_jit_call", rvm_jit_call as *const u8);
    builder.symbol("rvm_jit_halt", rvm_jit_halt as *const u8);
    builder.symbol("rvm_jit_brk", rvm_jit_brk as *const u8);
    builder.symbol("rvm_jit_unknown", rvm_jit_unknown as *const u8);
    let mut module = JITModule::new(builder);
    let ids = HostIds {
        load: import(&mut module, "rvm_jit_load", &[types::I32, types::I32], true)?,
        store: import(
            &mut module,
            "rvm_jit_store",
            &[types::I32, types::I32, types::I32],
            true,
        )?,
        loadc: import(
            &mut module,
            "rvm_jit_loadc",
            &[types::I32, types::I32],
            true,
        )?,
        storc: import(
            &mut module,
            "rvm_jit_storc",
            &[types::I32, types::I32],
            true,
        )?,
        call: import(&mut module, "rvm_jit_call", &[types::I32], true)?,
        halt: import(&mut module, "rvm_jit_halt", &[], false)?,
        brk: import(&mut module, "rvm_jit_brk", &[], false)?,
        unknown: import(&mut module, "rvm_jit_unknown", &[types::I32], false)?,
    };
    Ok((module, ids))
}

fn import(
    module: &mut JITModule,
    name: &str,
    extra: &[cranelift_codegen::ir::Type],
    ret: bool,
) -> Result<FuncId, String> {
    let mut sig = module.make_signature();
    sig.params
        .push(AbiParam::new(module.target_config().pointer_type()));
    for ty in extra {
        sig.params.push(AbiParam::new(*ty));
    }
    if ret {
        sig.returns.push(AbiParam::new(types::I32));
    }
    module
        .declare_function(name, Linkage::Import, &sig)
        .map_err(|err| format!("JIT import {name} failed: {err}"))
}

/// The pointer is the VM that entered the block. The block holds no Rust
/// reference across the call.
extern "C" fn rvm_jit_load(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.read_cell(bank as u16, addr as u16) {
        Ok(value) => i32::from(value),
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_store(vm: *mut VM, bank: i32, addr: i32, value: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    let bank = bank as u16;
    let addr = addr as u16;
    let boundary = bank == 0 && usize::from(addr) < TEXT40_BASE_WORD;
    match vm.write_cell(bank, addr, value as u16) {
        Ok(()) if boundary => 1,
        Ok(()) => 0,
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_loadc(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.guest_loadc(bank as u16, addr as u16) {
        Ok(()) => 0,
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

extern "C" fn rvm_jit_storc(vm: *mut VM, bank: i32, addr: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    match vm.guest_storc(bank as u16, addr as u16) {
        Ok(()) => 0,
        Err(error) => {
            vm.jit_fault = Some(error);
            -1
        }
    }
}

/// Look up the callee and run it. Out-of-range targets return to the
/// dispatcher, which reports the same PC error as the interpreter.
extern "C" fn rvm_jit_call(vm: *mut VM, index: i32) -> i32 {
    let vm = unsafe { &mut *vm };
    if index < 0 {
        vm.jit_fault = Some(format!("JIT call target {index} is negative"));
        return FAULT;
    }
    let index = index as usize;
    if index >= vm.instructions.len() {
        return CONT;
    }
    match vm.jit_block(index) {
        Ok(code) => unsafe { code(vm) },
        Err(error) => {
            vm.jit_fault = Some(error);
            FAULT
        }
    }
}

extern "C" fn rvm_jit_halt(vm: *mut VM) {
    unsafe { &mut *vm }.guest_halt();
}

extern "C" fn rvm_jit_brk(vm: *mut VM) {
    unsafe { &mut *vm }.guest_brk();
}

extern "C" fn rvm_jit_unknown(vm: *mut VM, opcode: i32) {
    let vm = unsafe { &mut *vm };
    vm.jit_fault = Some(format!("Unknown opcode: 0x{opcode:02X}"));
}

impl VM {
    pub(crate) fn jit_flush(&mut self) {
        self.jit_image_ready = false;
        if let Some(engine) = self.jit.as_mut() {
            engine.blocks.clear();
            engine.dead = engine.dead.saturating_add(1);
        }
    }

    pub(crate) fn jit_invalidate_index(&mut self, idx: usize) {
        if let Some(engine) = self.jit.as_mut() {
            engine.invalidate(idx);
        }
    }

    pub(super) fn run_batch_jit(&mut self, budget: u64) -> Result<u64, String> {
        if budget == 0 {
            return Ok(0);
        }
        match &self.state {
            super::VMState::Running => {}
            super::VMState::Setup => return Err("VM not initialized".into()),
            super::VMState::Error(error) => return Err(error.clone()),
            super::VMState::Halted | super::VMState::Breakpoint => return Ok(0),
        }
        if self.devices.has_pending_tick() || self.device_error.is_some() {
            return Ok(0);
        }
        self.precompile_jit()?;
        let before = self.executed_instructions;
        self.jit_budget = budget;
        self.jit_fault = None;
        while self.jit_budget > 0 {
            if self.devices.has_pending_tick() || self.device_error.is_some() {
                break;
            }
            match &self.state {
                super::VMState::Running => {}
                super::VMState::Halted | super::VMState::Breakpoint => break,
                super::VMState::Error(error) => return Err(error.clone()),
                super::VMState::Setup => return Err("VM not initialized".into()),
            }
            self.maybe_dispatch_irq();
            let pc = self.registers[Register::Pc as usize];
            let pcb = self.registers[Register::Pcb as usize];
            let instr_idx = (pcb as usize * self.bank_size as usize) + pc as usize;
            if instr_idx >= self.instructions.len() {
                self.state = super::VMState::Error(format!(
                    "PC out of bounds: bank={pcb}, offset={pc}, idx={instr_idx}, total_instructions={}",
                    self.instructions.len()
                ));
                return Err(format!(
                    "PC out of bounds: idx={instr_idx} >= len={}",
                    self.instructions.len()
                ));
            }
            let code = self.jit_block(instr_idx)?;
            let vm = self as *mut VM;
            let status = unsafe { code(vm) };
            if let Some(engine) = self.jit.as_mut() {
                engine.sweep()?;
            }
            if let Some(error) = self.jit_fault.take() {
                return Err(error);
            }
            match status {
                CONT => {}
                YIELD => break,
                FAULT => return Err("JIT fault".into()),
                other => return Err(format!("JIT status {other}")),
            }
        }
        Ok(self.executed_instructions - before)
    }

    /// Functions are compiled when they are entered. Walking every leader in
    /// the linked runtime spends longer in register allocation than the guest
    /// spends running.
    fn precompile_jit(&mut self) -> Result<(), String> {
        if self.jit_image_ready || self.instructions.is_empty() {
            self.jit_image_ready = true;
            return Ok(());
        }
        self.jit_image_ready = true;
        Ok(())
    }

    fn jit_block(&mut self, index: usize) -> Result<JitFn, String> {
        if let Some(code) = self.jit.as_ref().and_then(|engine| engine.get(index)) {
            return Ok(code);
        }
        let region = build_region(self.bank_size, index, &self.instructions)?;
        if self.jit.is_none() {
            self.jit = Some(JitEngine::new()?);
        }
        self.jit
            .as_mut()
            .ok_or_else(|| "JIT engine missing".to_string())?
            .compile(self.bank_size, index, &region, self.instructions.len())
    }
}
