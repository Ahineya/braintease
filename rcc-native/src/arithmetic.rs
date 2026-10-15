use super::emit::{width, Cg};
use cranelift_codegen::ir::{condcodes::IntCC, types, InstBuilder, Value as ClValue};
use rcc_ir::ir::{IrBinaryOp as Op, IrType, Value};

impl Cg<'_, '_> {
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
