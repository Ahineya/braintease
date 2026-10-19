use super::*;

impl Cg<'_> {
    pub(super) fn mem_load(&mut self, inst: &Instr) {
        let Some(rd) = rx(inst.word1) else {
            self.counted();
            return;
        };
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            return;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            return;
        };
        let bank = self.reg(bank);
        let addr = self.reg(addr);
        let bank = self.zext(bank);
        let addr = self.zext(addr);
        let raw = self.call_memory(false, bank, addr, None);
        let fault = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, raw, 0);
        self.side_if(fault, FAULT);
        let value = self.b.ins().ireduce(types::I16, raw);
        self.set_reg(rd, value);
        self.counted();
    }

    pub(super) fn loadc(&mut self, inst: &Instr) {
        let Some(bank) = rx(inst.word2) else {
            self.counted();
            return;
        };
        let Some(addr) = rx(inst.word3) else {
            self.counted();
            return;
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
    }

    pub(super) fn emit_store_effect(&mut self, inst: &Instr) {
        if let (Some(src), Some(bank), Some(addr)) =
            (rx(inst.word1), rx(inst.word2), rx(inst.word3))
        {
            let value = self.reg(src);
            let bank = self.reg(bank);
            let addr = self.reg(addr);
            let value = self.zext(value);
            let bank = self.zext(bank);
            let addr = self.zext(addr);
            let raw = self.call_memory(true, bank, addr, Some(value));
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

    pub(super) fn emit_storc_end(&mut self, inst: &Instr) {
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

    /// A cached user translation is usable only with the current MMU epoch,
    /// matching virtual page, permissions and physical/virtual bounds. A miss
    /// retains the reference helper's precise faults, including COW writes.
    fn call_user_memory(
        &mut self,
        store: bool,
        bank: Value,
        addr: Value,
        value: Option<Value>,
    ) -> Value {
        use crate::mmu::{Mmu, TlbEntry, PAGE_CELLS, READ, TLB_SIZE, USER, WRITE};
        let mmu = std::mem::offset_of!(VM, mmu) as i32;
        let page = self
            .b
            .ins()
            .ushr_imm_u(addr, PAGE_CELLS.trailing_zeros() as i64);
        let hash = self.b.ins().bxor(bank, page);
        let index = self.b.ins().band_imm_u(hash, (TLB_SIZE - 1) as i64);
        let index = self.b.ins().uextend(self.ptr, index);
        let bytes = self
            .b
            .ins()
            .imul_imm_s(index, std::mem::size_of::<TlbEntry>() as i64);
        let cache = self.b.ins().iadd_imm_s(
            self.vm,
            i64::from(mmu) + std::mem::offset_of!(Mmu, tlb) as i64,
        );
        let entry = self.b.ins().iadd(cache, bytes);
        let bank_tag = self.b.ins().ishl_imm_u(bank, 8);
        let page_tag = self.b.ins().ishl_imm_u(page, 1);
        let tag = self.b.ins().bor(bank_tag, page_tag);
        let tag = self.b.ins().iadd_imm_s(tag, 1);
        let cached_tag = self.b.ins().load(
            types::I32,
            self.flags,
            entry,
            std::mem::offset_of!(TlbEntry, tag) as i32,
        );
        let mut valid = self.b.ins().icmp(IntCC::Equal, tag, cached_tag);
        let zero_bank = self.b.ins().icmp_imm_s(IntCC::Equal, bank, 0);
        let math_offset = self.b.ins().icmp_imm_s(IntCC::Equal, addr, crate::constants::HDR_MATH_STACK as i64);
        let math = self.b.ins().band(zero_bank, math_offset);
        let ram = self.b.ins().bnot(math);
        valid = self.b.ins().band(valid, ram);
        let epoch = self.b.ins().load(
            types::I32,
            self.flags,
            self.vm,
            mmu + std::mem::offset_of!(Mmu, tlb_generation) as i32,
        );
        let cached_epoch = self.b.ins().load(
            types::I32,
            self.flags,
            entry,
            std::mem::offset_of!(TlbEntry, generation) as i32,
        );
        let same = self.b.ins().icmp(IntCC::Equal, epoch, cached_epoch);
        valid = self.b.ins().band(valid, same);
        let flags = self.b.ins().load(
            types::I32,
            self.flags,
            entry,
            std::mem::offset_of!(TlbEntry, flags) as i32,
        );
        let required = u64::from(USER | if store { WRITE } else { READ });
        let flags = self.b.ins().band_imm_u(flags, required as i64);
        let allowed = self
            .b
            .ins()
            .icmp_imm_s(IntCC::Equal, flags, required as i64);
        valid = self.b.ins().band(valid, allowed);
        let inside =
            self.b
                .ins()
                .icmp_imm_s(IntCC::UnsignedLessThan, addr, i64::from(self.bank_size));
        valid = self.b.ins().band(valid, inside);
        let within = self.b.ins().band_imm_u(addr, (PAGE_CELLS - 1) as i64);
        let span = self.b.ins().load(
            types::I32,
            self.flags,
            entry,
            std::mem::offset_of!(TlbEntry, span) as i32,
        );
        let inside = self.b.ins().icmp(IntCC::UnsignedLessThan, within, span);
        valid = self.b.ins().band(valid, inside);
        let base = self.b.ins().load(
            self.ptr,
            self.flags,
            entry,
            std::mem::offset_of!(TlbEntry, physical) as i32,
        );
        let within = self.b.ins().uextend(self.ptr, within);
        let index = self.b.ins().iadd(base, within);
        let length = self.memory_len.expect("memory prologue");
        let inside = self.b.ins().icmp(IntCC::UnsignedLessThan, index, length);
        valid = self.b.ins().band(valid, inside);
        let fast = self.b.create_block();
        let slow = self.b.create_block();
        let join = self.b.create_block();
        self.b.append_block_param(join, types::I32);
        self.b.ins().brif(valid, fast, no_args(), slow, no_args());
        self.b.switch_to_block(fast);
        let bytes = self.b.ins().ishl_imm_u(index, 1);
        let pointer = self
            .b
            .ins()
            .iadd(self.memory_base.expect("memory prologue"), bytes);
        let result = if store {
            let word = self
                .b
                .ins()
                .ireduce(types::I16, value.expect("store value"));
            self.b.ins().store(self.flags, word, pointer, 0);
            self.k32(0)
        } else {
            let word = self.b.ins().load(types::I16, self.flags, pointer, 0);
            self.zext(word)
        };
        self.b.ins().jump(join, &[BlockArg::Value(result)]);
        self.b.switch_to_block(slow);
        let result = if store {
            let sb = self.reg(Register::Sb as usize);
            self.store16(self.off.regs + Register::Sb as i32 * 2, sb);
            self.call_i32(
                self.store,
                &[self.vm, bank, addr, value.expect("store value")],
            )
        } else {
            self.call_i32(self.load, &[self.vm, bank, addr])
        };
        self.b.ins().jump(join, &[BlockArg::Value(result)]);
        self.b.switch_to_block(join);
        self.b.block_params(join)[0]
    }

    /// Bank zero retains all MMIO and legacy display routing. Ordinary RAM
    /// uses a checked native load/store, with the interpreter helper preserving
    /// the exact diagnostic on out-of-bounds accesses.
    pub(super) fn call_memory(
        &mut self,
        store: bool,
        bank: Value,
        addr: Value,
        value: Option<Value>,
    ) -> Value {
        if self.protected && self.user {
            return self.call_user_memory(store, bank, addr, value);
        }
        let nonzero_bank = self.b.ins().icmp_imm_s(IntCC::NotEqual, bank, 0);
        let bank64 = self.b.ins().uextend(types::I64, bank);
        let addr64 = self.b.ins().uextend(types::I64, addr);
        let scaled = self.b.ins().imul_imm_s(bank64, i64::from(self.bank_size));
        let index = self.b.ins().iadd(scaled, addr64);
        let length = self.memory_len.expect("memory prologue");
        let inside = self.b.ins().icmp(IntCC::UnsignedLessThan, index, length);
        let mut direct = self.b.ins().band(nonzero_bank, inside);
        if self.protected {
            let enabled = self
                .b
                .ins()
                .load(types::I8, self.flags, self.vm, self.off.window);
            let enabled = self.b.ins().icmp_imm_s(IntCC::NotEqual, enabled, 0);
            let lo = self.load16(self.off.window_lo);
            let hi = self.load16(self.off.window_hi);
            let lo = self.zext(lo);
            let hi = self.zext(hi);
            let above = self
                .b
                .ins()
                .icmp(IntCC::UnsignedGreaterThanOrEqual, bank, lo);
            let below = self.b.ins().icmp(IntCC::UnsignedLessThanOrEqual, bank, hi);
            let in_window = self.b.ins().band(above, below);
            let mapped = self.b.ins().band(enabled, in_window);
            let physical = self.b.ins().bnot(mapped);
            direct = self.b.ins().band(direct, physical);
        }
        let fast = self.b.create_block();
        let slow = self.b.create_block();
        let join = self.b.create_block();
        self.b.append_block_param(join, types::I32);
        self.b.ins().brif(direct, fast, no_args(), slow, no_args());
        self.b.switch_to_block(fast);
        let offset = self.b.ins().ishl_imm_u(index, 1);
        let offset = if self.ptr == types::I64 {
            offset
        } else {
            self.b.ins().ireduce(self.ptr, offset)
        };
        let base = self.memory_base.expect("memory prologue");
        let pointer = self.b.ins().iadd(base, offset);
        let result = if store {
            let word = self
                .b
                .ins()
                .ireduce(types::I16, value.expect("store value"));
            self.b.ins().store(self.flags, word, pointer, 0);
            self.k32(0)
        } else {
            let word = self.b.ins().load(types::I16, self.flags, pointer, 0);
            self.zext(word)
        };
        self.b.ins().jump(join, &[BlockArg::Value(result)]);
        self.b.switch_to_block(slow);
        let result = if store {
            let sb = self.reg(Register::Sb as usize);
            self.store16(self.off.regs + Register::Sb as i32 * 2, sb);
            self.call_i32(
                self.store,
                &[self.vm, bank, addr, value.expect("store value")],
            )
        } else {
            self.call_i32(self.load, &[self.vm, bank, addr])
        };
        self.b.ins().jump(join, &[BlockArg::Value(result)]);
        self.b.switch_to_block(join);
        self.b.block_params(join)[0]
    }
}
