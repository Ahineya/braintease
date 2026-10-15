use super::*;

impl Cg<'_> {
    pub(super) fn emit_branch_edge(
        &mut self,
        inst: &Instr,
        taken: Dest,
        fall: Dest,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
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

    pub(super) fn emit_halt(&mut self) {
        self.b.ins().call(self.halt, &[self.vm]);
        self.bump();
        self.advance();
        self.finish(YIELD);
    }

    pub(super) fn emit_brk(&mut self) {
        self.spill();
        self.b.ins().call(self.brk, &[self.vm]);
        self.bump();
        self.advance();
        self.finish(YIELD);
    }

    pub(super) fn emit_jal_effect(&mut self, inst: &Instr) {
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

    /// Missing callees return to the driver, which owns warmup and compilation.
    /// Native code never invokes the compiler while holding native call frames.
    pub(super) fn call_guest(&mut self, target: usize) -> Value {
        let cell = self.b.ins().iconst(self.ptr, self.table_cell);
        let base = self.b.ins().load(self.ptr, self.flags, cell, 0);
        let offset = target as i64 * i64::from(self.ptr.bytes());
        let slot = self.b.ins().iadd_imm_s(base, offset);
        let fp = self.b.ins().load(self.ptr, self.flags, slot, 0);
        let missing = self.b.ins().icmp_imm_s(IntCC::Equal, fp, 0);
        self.side_if(missing, CONT);
        let call = self.b.ins().call_indirect(self.guest_sig, fp, &[self.vm]);
        self.b.inst_results(call)[0]
    }

    /// Publish registers, call the callee, and resume unless it yielded or
    /// faulted. The callee has its own variables and reads the VM.
    pub(super) fn emit_native_call(
        &mut self,
        inst: &Instr,
        dest: Dest,
        blocks: &HashMap<usize, Block>,
    ) -> Result<(), String> {
        if inst.word3 >= self.bank_size
            || linear(self.bank_size, inst.word3, inst.word2) >= self.table_len
        {
            self.finish(CONT);
            return Ok(());
        }
        let budget = self.b.use_var(self.var_budget);
        let empty = self.b.ins().icmp_imm_s(IntCC::Equal, budget, 0);
        self.side_if(empty, YIELD);

        // Guest recursion may be deeper than the host stack. Continue through
        // the driver when this bounded native call chain is full.
        let depth = self.load64(self.off.depth);
        let full = self
            .b
            .ins()
            .icmp_imm_s(IntCC::UnsignedGreaterThanOrEqual, depth, 64);
        self.side_if(full, CONT);
        let epoch = self.load64(self.off.epoch);
        self.spill();
        let target = linear(self.bank_size, inst.word3, inst.word2);
        let status = self.call_guest(target);
        let stop = self.b.ins().icmp_imm_s(IntCC::NotEqual, status, 0);
        let cont = self.b.create_block();
        let side = self.b.create_block();
        self.b.ins().brif(stop, side, no_args(), cont, no_args());
        self.b.switch_to_block(side);
        self.native_return(status);
        self.b.switch_to_block(cont);
        self.reload_all();
        let current_epoch = self.load64(self.off.epoch);
        let changed = self.b.ins().icmp(IntCC::NotEqual, current_epoch, epoch);
        self.side_if(changed, CONT);
        let pc = self.reg(Register::Pc as usize);
        let noncanonical = self.b.ins().icmp_imm_s(
            IntCC::UnsignedGreaterThanOrEqual,
            pc,
            i64::from(self.bank_size),
        );
        self.side_if(noncanonical, CONT);
        self.resume_at(dest, blocks)
    }

    pub(super) fn resume_at(
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
        self.b.ins().brif(same, block, no_args(), other, no_args());
        self.b.switch_to_block(other);
        self.b.ins().jump(resume, no_args());
        Ok(())
    }

    pub(super) fn reload_all(&mut self) {
        let zero = self.k16(0);
        self.b.def_var(self.var_reg[0], zero);
        self.store16(self.reg_off(0), zero);
        for index in 1..32 {
            self.reload(index);
        }
        let executed = self.load64(self.off.executed);
        let budget = self.load64(self.off.budget);
        self.b.def_var(self.var_executed, executed);
        self.b.def_var(self.var_budget, budget);
    }

    pub(super) fn emit_jalr(&mut self, inst: &Instr) {
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

    pub(super) fn emit_unknown(&mut self, opcode: u8) {
        let code = self.k32(i64::from(opcode));
        self.b.ins().call(self.unknown, &[self.vm, code]);
        self.finish(FAULT);
    }
}
