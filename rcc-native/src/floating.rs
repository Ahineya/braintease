//! Hardware floating-point selection. Integer carriers exist only at the v1
//! memory/call boundary; all numeric operations below use native FP values.
use super::emit::Cg;
use cranelift_codegen::ir::{
    condcodes::FloatCC, types, InstBuilder, MemFlagsData, Value as ClValue,
};
use rcc_ir::{
    float::{FloatIntrinsic, FloatOp as Op, Precision},
    ir::{IrType, Value},
};

impl Cg<'_, '_> {
    fn as_float(&mut self, value: ClValue, precision: Precision) -> ClValue {
        let (integer, float) = match precision {
            Precision::F32 => (types::I32, types::F32),
            Precision::F64 => (types::I64, types::F64),
        };
        let value = if integer == types::I32 {
            self.b.ins().ireduce(integer, value)
        } else {
            value
        };
        self.b.ins().bitcast(float, MemFlagsData::new(), value)
    }
    fn float_bits(&mut self, value: ClValue, precision: Precision) -> ClValue {
        let integer = if precision == Precision::F32 {
            types::I32
        } else {
            types::I64
        };
        let bits = self.b.ins().bitcast(integer, MemFlagsData::new(), value);
        if integer == types::I32 {
            self.b.ins().uextend(types::I64, bits)
        } else {
            bits
        }
    }
    pub fn floating(
        &mut self,
        name: &str,
        args: &[Value],
        result_type: &IrType,
    ) -> Result<ClValue, String> {
        if let Some(bits) = rcc_ir::float::reinterpret_type(name) {
            if args.len() != 1
                || Some(result_type.clone()) != rcc_ir::float::reinterpret_result(name)
            {
                return Err(format!("invalid {name} signature"));
            }
            if let Value::Temp(id) = &args[0] {
                let source = self
                    .types
                    .get(id)
                    .ok_or_else(|| format!("undefined IR temporary %{id}"))?;
                let typed = name.starts_with("float.bits.");
                let float = if bits == IrType::I32 {
                    IrType::F32
                } else {
                    IrType::F64
                };
                if *source != float && (typed || *source != bits) {
                    return Err(format!(
                        "{name} cannot reinterpret operand of type {source}"
                    ));
                }
            }
            let value = self.value(&args[0])?;
            return Ok(self.mask(value, super::emit::width(&bits)));
        }
        let spec = FloatIntrinsic::parse(name)
            .ok_or_else(|| format!("unknown native intrinsic {name}"))?;
        spec.validate(args, result_type)?;
        for (arg, expected) in args.iter().zip(spec.params()) {
            if let Value::Temp(id) = arg {
                let source = self
                    .types
                    .get(id)
                    .ok_or_else(|| format!("undefined IR temporary %{id}"))?;
                if *source != expected {
                    return Err(format!("{name} expects {expected}, got {source}"));
                }
            }
        }
        let precision = spec.precision;
        let ty = if precision == Precision::F32 {
            types::F32
        } else {
            types::F64
        };
        let a = self.value(&args[0])?;
        let numeric = match spec.op {
            Op::FromSI | Op::FromUI | Op::FromI64 | Op::FromU64 => {
                let wide = matches!(spec.op, Op::FromI64 | Op::FromU64);
                let signed = matches!(spec.op, Op::FromSI | Op::FromI64);
                let integer = if wide {
                    a
                } else {
                    self.b.ins().ireduce(types::I32, a)
                };
                if signed {
                    self.b.ins().fcvt_from_sint(ty, integer)
                } else {
                    self.b.ins().fcvt_from_uint(ty, integer)
                }
            }
            Op::FromF32 => {
                let a = self.as_float(a, Precision::F32);
                self.b.ins().fpromote(types::F64, a)
            }
            Op::FromF64 => {
                let a = self.as_float(a, Precision::F64);
                self.b.ins().fdemote(types::F32, a)
            }
            Op::ToSI | Op::ToUI | Op::ToI64 | Op::ToU64 => {
                let a = self.as_float(a, precision);
                let ty = if matches!(spec.op, Op::ToI64 | Op::ToU64) {
                    types::I64
                } else {
                    types::I32
                };
                let value = if matches!(spec.op, Op::ToSI | Op::ToI64) {
                    self.b.ins().fcvt_to_sint_sat(ty, a)
                } else {
                    self.b.ins().fcvt_to_uint_sat(ty, a)
                };
                return Ok(if ty == types::I32 {
                    self.b.ins().uextend(types::I64, value)
                } else {
                    value
                });
            }
            _ => {
                let b = self.value(&args[1])?;
                let a = self.as_float(a, precision);
                let b = self.as_float(b, precision);
                match spec.op {
                    Op::Add => self.b.ins().fadd(a, b),
                    Op::Sub => self.b.ins().fsub(a, b),
                    Op::Mul => self.b.ins().fmul(a, b),
                    Op::Div => self.b.ins().fdiv(a, b),
                    _ => {
                        let cc = match spec.op {
                            Op::Eq => FloatCC::Equal,
                            Op::Ne => FloatCC::NotEqual,
                            Op::Lt => FloatCC::LessThan,
                            Op::Le => FloatCC::LessThanOrEqual,
                            Op::Gt => FloatCC::GreaterThan,
                            Op::Ge => FloatCC::GreaterThanOrEqual,
                            _ => unreachable!(),
                        };
                        let value = self.b.ins().fcmp(cc, a, b);
                        return Ok(self.b.ins().uextend(types::I64, value));
                    }
                }
            }
        };
        Ok(self.float_bits(numeric, precision))
    }
}
