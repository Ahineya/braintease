//! Integer-only translation. Basic-block budget guards retain precise exits.
mod arithmetic;
mod control;
mod memory;

use super::region::*;
use super::{JitEngine, CONT, FALLBACK, FAULT, VM, YIELD};
use crate::vm::Instr;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::{
    types, Block, BlockArg, FuncRef, InstBuilder, MemFlagsData, SigRef, Type, Value,
};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Switch, Variable};
use cranelift_module::Module;
use ripple_asm::Register;
use std::collections::HashMap;

struct Offsets {
    regs: i32,
    executed: i32,
    budget: i32,
    depth: i32,
    epoch: i32,
    memory_base: i32,
    memory_len: i32,
}

fn offsets() -> Offsets {
    Offsets {
        regs: i32_offset(std::mem::offset_of!(VM, registers)),
        executed: i32_offset(std::mem::offset_of!(VM, executed_instructions)),
        budget: i32_offset(std::mem::offset_of!(VM, jit_budget)),
        depth: i32_offset(std::mem::offset_of!(VM, jit_depth)),
        epoch: i32_offset(std::mem::offset_of!(VM, jit_epoch)),
        memory_base: i32_offset(std::mem::offset_of!(VM, jit_memory_base)),
        memory_len: i32_offset(std::mem::offset_of!(VM, jit_memory_len)),
    }
}

fn i32_offset(bytes: usize) -> i32 {
    i32::try_from(bytes).unwrap_or_else(|_| panic!("VM field offset {bytes} exceeds i32"))
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

impl JitEngine {
    pub(super) fn translate(
        &mut self,
        ctx: &mut cranelift_codegen::Context,
        bank_size: u16,
        region: &Region,
        code_len: usize,
    ) -> Result<(), String> {
        let ptr = self.module.target_config().pointer_type();
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
            let load = self.module.declare_func_in_func(ids.load, builder.func);
            let store = self.module.declare_func_in_func(ids.store, builder.func);
            let loadc = self.module.declare_func_in_func(ids.loadc, builder.func);
            let storc = self.module.declare_func_in_func(ids.storc, builder.func);
            let halt = self.module.declare_func_in_func(ids.halt, builder.func);
            let brk = self.module.declare_func_in_func(ids.brk, builder.func);
            let unknown = self.module.declare_func_in_func(ids.unknown, builder.func);
            let guest_sig = builder.func.import_signature(guest_sig_data);
            let var_reg = std::array::from_fn(|_| builder.declare_var(types::I16));
            let var_executed = builder.declare_var(types::I64);
            let var_budget = builder.declare_var(types::I64);
            let mut cg = Cg {
                b: builder,
                vm,
                entry_depth: None,
                memory_base: None,
                memory_len: None,
                var_reg,
                modified: modified_registers(region),
                var_executed,
                var_budget,
                yield_exit: None,
                fault_exit: None,
                cont_exit: None,
                fallback_exit: None,
                resume: None,
                bank_size,
                next_pc: None,
                flags: MemFlagsData::trusted(),
                off: offsets(),
                load,
                store,
                loadc,
                storc,
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
        Ok(())
    }
}

struct Cg<'a> {
    b: FunctionBuilder<'a>,
    vm: Value,
    entry_depth: Option<Value>,
    memory_base: Option<Value>,
    memory_len: Option<Value>,
    var_reg: [Variable; 32],
    modified: [bool; 32],
    var_executed: Variable,
    var_budget: Variable,
    yield_exit: Option<Block>,
    fault_exit: Option<Block>,
    cont_exit: Option<Block>,
    fallback_exit: Option<Block>,
    /// Dispatch block. A call returns here when the guest PC is not the
    /// instruction immediately after the call.
    resume: Option<Block>,
    bank_size: u16,
    next_pc: Option<(u16, u16)>,
    flags: MemFlagsData,
    off: Offsets,
    load: FuncRef,
    store: FuncRef,
    loadc: FuncRef,
    storc: FuncRef,
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
        let depth = self.load64(self.off.depth);
        self.entry_depth = Some(depth);
        self.memory_base = Some(self.b.ins().load(
            self.ptr,
            self.flags,
            self.vm,
            self.off.memory_base,
        ));
        self.memory_len = Some(self.load64(self.off.memory_len));
        let next_depth = self.b.ins().iadd_imm_s(depth, 1);
        self.store64(self.off.depth, next_depth);
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
            (self.fallback_exit, FALLBACK),
        ];
        for (block, status) in exits {
            let Some(block) = block else {
                continue;
            };
            self.b.switch_to_block(block);
            self.spill();
            let status = self.k32(i64::from(status));
            self.native_return(status);
        }
    }

    /// The same native function is entered at every block in the region.
    fn dispatch(&mut self, region: &Region, blocks: &HashMap<usize, Block>) {
        let here = self.guest_index();
        let mut switch = Switch::new();
        for block in &region.blocks {
            switch.set_entry(block.index as u128, blocks[&block.index]);
        }
        let outside = self.b.create_block();
        switch.emit(&mut self.b, here, outside);
        self.b.switch_to_block(outside);
        self.finish(CONT);
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
        self.store16(self.reg_off(0), zero);
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
        self.instruction_pc(block.index, &block.instrs[0]);
        self.ensure_block_budget(block.instrs.len());
        let last = block.instrs.len() - 1;
        for (offset, inst) in block.instrs[..last].iter().enumerate() {
            self.instruction_pc(block.index + offset, inst);
            self.emit_inner(inst);
        }
        let inst = &block.instrs[last];
        self.instruction_pc(block.index + last, inst);
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
                if inst.word1 == 0 || inst.word1 >= 32 {
                    self.take(dest, blocks)
                } else {
                    self.emit_native_call(inst, dest, blocks)
                }
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

    fn instruction_pc(&mut self, index: usize, inst: &Instr) {
        let (pc, pcb) = locate(self.bank_size, index);
        let pc_value = self.k16(pc);
        let pcb_value = self.k16(pcb);
        self.b
            .def_var(self.var_reg[Register::Pc as usize], pc_value);
        self.b
            .def_var(self.var_reg[Register::Pcb as usize], pcb_value);
        self.next_pc = (!writes_pc(inst)).then(|| step_pc(self.bank_size, pc, pcb));
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

    fn emit_inner(&mut self, inst: &Instr) {
        if inst.opcode == 0x12 {
            self.emit_store_effect(inst);
        } else {
            self.emit_plain(inst);
        }
    }

    fn counted(&mut self) {
        self.bump();
        self.advance();
    }

    fn ensure_block_budget(&mut self, length: usize) {
        let budget = self.b.use_var(self.var_budget);
        let short = self
            .b
            .ins()
            .icmp_imm_s(IntCC::UnsignedLessThan, budget, length as i64);
        self.side_if(short, FALLBACK);
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
        if let Some((pc, pcb)) = self.next_pc {
            let pc = self.k16(pc);
            let pcb = self.k16(pcb);
            self.set_reg(Register::Pc as usize, pc);
            self.set_reg(Register::Pcb as usize, pcb);
            return;
        }
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

    fn native_return(&mut self, status: Value) {
        let depth = self.entry_depth.expect("native prologue");
        self.store64(self.off.depth, depth);
        self.b.ins().return_(&[status]);
    }

    fn finish(&mut self, status: i32) {
        let exit = self.exit_for(status);
        self.b.ins().jump(exit, no_args());
    }

    fn exit_for(&mut self, status: i32) -> Block {
        let slot = match status {
            YIELD => &mut self.yield_exit,
            FAULT => &mut self.fault_exit,
            FALLBACK => &mut self.fallback_exit,
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
        for index in 1..32 {
            if !self.modified[index] {
                continue;
            }
            let value = self.reg(index);
            self.store16(self.reg_off(index), value);
        }
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
        if index != 0 && index < 32 {
            self.b.def_var(self.var_reg[index], value);
        }
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
        self.off.regs + i32::try_from(index * 2).expect("register offset fits in i32")
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

/// A region publishes only registers it can modify. Callees publish their own
/// changes; reload_all reads those results before the caller resumes.
fn modified_registers(region: &Region) -> [bool; 32] {
    let mut modified = [false; 32];
    modified[Register::Pc as usize] = true;
    modified[Register::Pcb as usize] = true;
    for inst in region.blocks.iter().flat_map(|block| &block.instrs) {
        if inst.opcode == 0x20 {
            modified[Register::X0 as usize..=Register::X3 as usize].fill(true);
        } else if matches!(inst.opcode, 0x01..=0x11 | 0x1A..=0x1F | 0x13 | 0x14) {
            if let Some(rd) = rx(inst.word1).filter(|&rd| rd != 0) {
                modified[rd] = true;
                if matches!(inst.opcode, 0x13 | 0x14) {
                    modified[Register::Rab as usize] = true;
                }
            }
        }
    }
    modified
}
