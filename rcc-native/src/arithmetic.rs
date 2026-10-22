use super::emit::{width, Cg};
use cranelift_codegen::ir::{condcodes::IntCC, types, InstBuilder, Value as ClValue};
use rcc_ir::ir::{IrBinaryOp as Op, IrType, Value};

impl Cg<'_, '_> {
    /// Normalize the common same-bank case without entering the runtime.
    /// Both hosted and freestanding runtimes back the original 256 banks.
    /// Hosted read-v1 additionally requires the expanded 65536-bank runtime.
    /// Bank crossings and malformed inputs retain the checked
    /// helper, including its target-specific limits and diagnostics. GEP does
    /// not access memory or poll; this adds no COW/view/scheduling obligations.
    pub(super) fn logical_gep(
        &mut self,
        pointer: ClValue,
        delta: ClValue,
    ) -> Result<ClValue, String> {
        let bank_cells = i64::from(rcc_ir::target::DataLayout::RIPPLE16.bank_cells);
        let bank = self.b.ins().ushr_imm_u(pointer, 16);
        let offset = self.b.ins().band_imm_u(pointer, 65535);
        let next = self.b.ins().iadd(offset, delta);
        let bank_limit = if self.compiler.read_views { 65536 } else { 256 };
        let valid_bank = self.b.ins().icmp_imm_u(IntCC::UnsignedLessThan, bank, bank_limit);
        let valid_offset = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, offset, bank_cells);
        // Unsigned comparison also rejects negative results and extreme signed
        // deltas. With offset < 64000, adding any i64 delta can only wrap into
        // this small accepted range when the mathematical result is valid.
        let same_bank = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, next, bank_cells);
        let valid = self.b.ins().band(valid_bank, valid_offset);
        let valid = self.b.ins().band(valid, same_bank);
        let bank_bits = self.b.ins().band_imm_u(pointer, -65536);
        let fast = self.b.ins().bor(bank_bits, next);

        let slow = self.b.create_block();
        let done = self.b.create_block();
        self.b.append_block_param(done, types::I64);
        // Only the inline branch retains a view. The helper branch is a call
        // barrier even though this particular checked GEP does not read RAM.
        let view = self.read_view;
        let mut fast_args = vec![fast.into()];
        if let Some(view) = view {
            for _ in 0..4 { self.b.append_block_param(done, types::I64); }
            fast_args.extend(view.values().map(cranelift_codegen::ir::BlockArg::from));
        }
        self.b.ins().brif(valid, done, &fast_args, slow, &[]);
        self.b.switch_to_block(slow);
        let checked = self.runtime("rcc_native_gep", &[pointer, delta])?;
        let mut slow_args = vec![checked.into()];
        if view.is_some() {
            let zero = self.k(0);
            slow_args.extend([cranelift_codegen::ir::BlockArg::from(zero); 4]);
        }
        self.b.ins().jump(done, &slow_args);
        self.b.switch_to_block(done);
        self.read_view = view.map(|_| super::memory::ReadView::from_values(&self.b.block_params(done)[1..]));
        Ok(self.b.block_params(done)[0])
    }

    pub fn width_of(&self, value: &Value) -> u32 {
        match value {
            Value::Temp(t) => self.types.get(t).map(width).unwrap_or(16),
            Value::FatPtr(_)
            | Value::Global(_)
            | Value::Function(_)
            | Value::AddressOffset { .. } => 32,
            _ => 16,
        }
    }
    pub fn mask(&mut self, value: ClValue, bits: u32) -> ClValue {
        if bits == 64 {
            value
        } else {
            self.b.ins().band_imm_u(value, ((1u64 << bits) - 1) as i64)
        }
    }
    pub fn signed(&mut self, value: ClValue, bits: u32) -> ClValue {
        if bits == 64 {
            value
        } else {
            let value = self.b.ins().ishl_imm_u(value, 64 - bits as i64);
            self.b.ins().sshr_imm_u(value, 64 - bits as i64)
        }
    }
    pub fn binary(
        &mut self,
        op: Op,
        lhs: &Value,
        rhs: &Value,
        result_type: &IrType,
    ) -> Result<ClValue, String> {
        if result_type.is_fp() {
            return Err(
                "integer IR operations cannot produce a float; use float.* intrinsics".into(),
            );
        }
        let comparison = matches!(
            op,
            Op::Eq
                | Op::Ne
                | Op::Slt
                | Op::Sle
                | Op::Sgt
                | Op::Sge
                | Op::Ult
                | Op::Ule
                | Op::Ugt
                | Op::Uge
        );
        let bits = if comparison {
            match (lhs, rhs) {
                (Value::Constant(_), Value::Constant(_)) => 64,
                (Value::Constant(_), _) => self.width_of(rhs),
                (_, Value::Constant(_)) => self.width_of(lhs),
                _ => self.width_of(lhs).max(self.width_of(rhs)),
            }
        } else {
            width(result_type)
        };
        let left = self.value(lhs)?;
        let right = self.value(rhs)?;
        let a = self.mask(left, bits);
        let b = self.mask(right, bits);
        let signed = matches!(
            op,
            Op::SDiv | Op::SRem | Op::Slt | Op::Sle | Op::Sgt | Op::Sge | Op::AShr
        );
        let sa = if signed { self.signed(a, bits) } else { a };
        let sb = if signed { self.signed(b, bits) } else { b };
        let value = match op {
            Op::Add => self.b.ins().iadd(a, b),
            Op::Sub => self.b.ins().isub(a, b),
            Op::Mul => self.b.ins().imul(a, b),
            Op::And => self.b.ins().band(a, b),
            Op::Or => self.b.ins().bor(a, b),
            Op::Xor => self.b.ins().bxor(a, b),
            Op::Shl | Op::LShr | Op::AShr => {
                let count = self.b.ins().band_imm_u(b, i64::from(bits - 1));
                match op {
                    Op::Shl => self.b.ins().ishl(a, count),
                    Op::LShr => self.b.ins().ushr(a, count),
                    _ => self.b.ins().sshr(sa, count),
                }
            }
            Op::SDiv | Op::SRem | Op::UDiv | Op::URem => {
                let zero = self.b.ins().icmp_imm_s(IntCC::Equal, b, 0);
                let one = self.k(1);
                let divisor = self.b.ins().select(zero, one, if signed { sb } else { b });
                // Widened operations avoid narrow MIN/-1 traps. The I64 case
                // needs an explicit guard and wrapping result as well.
                let overflow = if signed && bits == 64 {
                    let min = self.k(i64::MIN);
                    let is_min = self.b.ins().icmp(IntCC::Equal, sa, min);
                    let minus_one = self.b.ins().icmp_imm_s(IntCC::Equal, sb, -1);
                    Some(self.b.ins().band(is_min, minus_one))
                } else {
                    None
                };
                let divisor = if let Some(overflow) = overflow {
                    self.b.ins().select(overflow, one, divisor)
                } else {
                    divisor
                };
                let value = match op {
                    Op::SDiv => self.b.ins().sdiv(sa, divisor),
                    Op::SRem => self.b.ins().srem(sa, divisor),
                    Op::UDiv => self.b.ins().udiv(a, divisor),
                    _ => self.b.ins().urem(a, divisor),
                };
                let value = if let Some(overflow) = overflow {
                    let replacement = if op == Op::SDiv {
                        self.k(i64::MIN)
                    } else {
                        self.k(0)
                    };
                    self.b.ins().select(overflow, replacement, value)
                } else {
                    value
                };
                let z = self.k(0);
                self.b.ins().select(zero, z, value)
            }
            _ => {
                let cc = match op {
                    Op::Eq => IntCC::Equal,
                    Op::Ne => IntCC::NotEqual,
                    Op::Slt => IntCC::SignedLessThan,
                    Op::Sle => IntCC::SignedLessThanOrEqual,
                    Op::Sgt => IntCC::SignedGreaterThan,
                    Op::Sge => IntCC::SignedGreaterThanOrEqual,
                    Op::Ult => IntCC::UnsignedLessThan,
                    Op::Ule => IntCC::UnsignedLessThanOrEqual,
                    Op::Ugt => IntCC::UnsignedGreaterThan,
                    Op::Uge => IntCC::UnsignedGreaterThanOrEqual,
                    _ => unreachable!(),
                };
                let value = self.b.ins().icmp(cc, sa, sb);
                self.b.ins().uextend(types::I64, value)
            }
        };
        Ok(self.mask(value, if comparison { width(result_type) } else { bits }))
    }
}
