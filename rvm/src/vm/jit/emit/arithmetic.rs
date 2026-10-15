use super::*;

impl Cg<'_> {
    pub(super) fn emit_plain(&mut self, inst: &Instr) {
        match inst.opcode {
            0x42 => {
                self.counted();
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
            }
            0x0F => self.ri(inst, |cg, rs, imm| {
                cg.b.ins().ishl_imm_u(rs, i64::from(imm & 15))
            }),
            0x10 => self.ri(inst, |cg, rs, imm| {
                cg.b.ins().ushr_imm_u(rs, i64::from(imm & 15))
            }),
            0x11 => self.mem_load(inst),
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
            opcode => unreachable!("non-plain opcode {opcode:#x} inside a basic block"),
        }
    }

    pub(super) fn rr(&mut self, inst: &Instr, op: impl FnOnce(&mut Self, Value, Value) -> Value) {
        if let (Some(rd), Some(rs), Some(rt)) = (rx(inst.word1), rx(inst.word2), rx(inst.word3)) {
            let rs = self.reg(rs);
            let rt = self.reg(rt);
            let value = op(self, rs, rt);
            self.set_reg(rd, value);
        }
        self.counted();
    }

    pub(super) fn ri(&mut self, inst: &Instr, op: impl FnOnce(&mut Self, Value, u16) -> Value) {
        if let (Some(rd), Some(rs)) = (rx(inst.word1), rx(inst.word2)) {
            let rs = self.reg(rs);
            let value = op(self, rs, inst.word3);
            self.set_reg(rd, value);
        }
        self.counted();
    }

    pub(super) fn flag(&mut self, cc: IntCC, left: Value, right: Value) -> Value {
        let bit = self.b.ins().icmp(cc, left, right);
        self.b.ins().uextend(types::I16, bit)
    }

    pub(super) fn masked_shift(&mut self, amount: Value) -> Value {
        let mask = self.k16(15);
        self.b.ins().band(amount, mask)
    }

    pub(super) fn divrem(&mut self, left: Value, right: Value, rem: bool) -> Value {
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

    pub(super) fn divrem_imm(&mut self, left: Value, imm: u16, rem: bool) -> Value {
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
}
