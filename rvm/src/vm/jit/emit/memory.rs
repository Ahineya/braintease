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
        let nonzero_bank = self.b.ins().icmp_imm_s(IntCC::NotEqual, bank, 0);
        let bank64 = self.b.ins().uextend(types::I64, bank);
        let addr64 = self.b.ins().uextend(types::I64, addr);
        let scaled = self.b.ins().imul_imm_s(bank64, i64::from(self.bank_size));
        let index = self.b.ins().iadd(scaled, addr64);
        let length = self.memory_len.expect("memory prologue");
        let inside = self.b.ins().icmp(IntCC::UnsignedLessThan, index, length);
        let direct = self.b.ins().band(nonzero_bank, inside);
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
