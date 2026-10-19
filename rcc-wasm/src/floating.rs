//! Typed IEEE operations execute in Wasm; scalar ABI carriers preserve bits.
use crate::emit::{width, Cg};
use rcc_ir::{
    float::{self, FloatIntrinsic, FloatOp as Op, Precision},
    ir::{IrType, Value},
};
use wasm_encoder::Instruction as W;
impl Cg<'_> {
    fn float_operand(&mut self, value: &Value, p: Precision) -> Result<(), String> {
        self.value(value)?;
        if p == Precision::F32 {
            self.ins(W::I32WrapI64);
            self.ins(W::F32ReinterpretI32);
        } else {
            self.ins(W::F64ReinterpretI64);
        }
        Ok(())
    }
    fn float_bits(&mut self, p: Precision) {
        if p == Precision::F32 {
            self.ins(W::I32ReinterpretF32);
            self.ins(W::I64ExtendI32U);
        } else {
            self.ins(W::I64ReinterpretF64);
        }
    }
    pub fn floating(
        &mut self,
        name: &str,
        args: &[Value],
        result_type: &IrType,
    ) -> Result<(), String> {
        if let Some(bits) = float::reinterpret_type(name) {
            if args.len() != 1 || Some(result_type.clone()) != float::reinterpret_result(name) {
                return Err(format!("invalid {name} signature"));
            }
            if let Value::Temp(id) = &args[0] {
                let actual = &self.locals.get(id).ok_or("undefined float operand")?.1;
                let float = if bits == IrType::I32 {
                    IrType::F32
                } else {
                    IrType::F64
                };
                if *actual != float && (name.starts_with("float.bits.") || *actual != bits) {
                    return Err(format!("{name} cannot reinterpret {actual}"));
                }
            }
            self.value(&args[0])?;
            self.mask(width(&bits)?);
            return Ok(());
        }
        let spec =
            FloatIntrinsic::parse(name).ok_or_else(|| format!("unknown Wasm intrinsic {name}"))?;
        spec.validate(args, result_type)?;
        for (arg, expected) in args.iter().zip(spec.params()) {
            if let Value::Temp(id) = arg {
                let actual = &self.locals.get(id).ok_or("undefined float operand")?.1;
                if *actual != expected {
                    return Err(format!("{name} expects {expected}, got {actual}"));
                }
            }
        }
        let p = spec.precision;
        let f32 = p == Precision::F32;
        match spec.op {
            Op::FromSI | Op::FromUI | Op::FromI64 | Op::FromU64 => {
                let wide = matches!(spec.op, Op::FromI64 | Op::FromU64);
                let signed = matches!(spec.op, Op::FromSI | Op::FromI64);
                self.value(&args[0])?;
                if !wide {
                    self.ins(W::I32WrapI64);
                }
                self.ins(match (f32, wide, signed) {
                    (true, false, true) => W::F32ConvertI32S,
                    (true, false, false) => W::F32ConvertI32U,
                    (true, true, true) => W::F32ConvertI64S,
                    (true, true, false) => W::F32ConvertI64U,
                    (false, false, true) => W::F64ConvertI32S,
                    (false, false, false) => W::F64ConvertI32U,
                    (false, true, true) => W::F64ConvertI64S,
                    (false, true, false) => W::F64ConvertI64U,
                });
                self.float_bits(p);
            }
            Op::FromF32 => {
                self.float_operand(&args[0], Precision::F32)?;
                self.ins(W::F64PromoteF32);
                self.float_bits(p);
            }
            Op::FromF64 => {
                self.float_operand(&args[0], Precision::F64)?;
                self.ins(W::F32DemoteF64);
                self.float_bits(p);
            }
            Op::ToSI | Op::ToUI | Op::ToI64 | Op::ToU64 => {
                let wide = matches!(spec.op, Op::ToI64 | Op::ToU64);
                let signed = matches!(spec.op, Op::ToSI | Op::ToI64);
                self.float_operand(&args[0], p)?;
                self.ins(match (f32, wide, signed) {
                    (true, false, true) => W::I32TruncSatF32S,
                    (true, false, false) => W::I32TruncSatF32U,
                    (true, true, true) => W::I64TruncSatF32S,
                    (true, true, false) => W::I64TruncSatF32U,
                    (false, false, true) => W::I32TruncSatF64S,
                    (false, false, false) => W::I32TruncSatF64U,
                    (false, true, true) => W::I64TruncSatF64S,
                    (false, true, false) => W::I64TruncSatF64U,
                });
                if !wide {
                    self.ins(W::I64ExtendI32U);
                }
            }
            op => {
                self.float_operand(&args[0], p)?;
                self.float_operand(&args[1], p)?;
                self.ins(match (f32, op) {
                    (true, Op::Add) => W::F32Add,
                    (true, Op::Sub) => W::F32Sub,
                    (true, Op::Mul) => W::F32Mul,
                    (true, Op::Div) => W::F32Div,
                    (false, Op::Add) => W::F64Add,
                    (false, Op::Sub) => W::F64Sub,
                    (false, Op::Mul) => W::F64Mul,
                    (false, Op::Div) => W::F64Div,
                    (true, Op::Eq) => W::F32Eq,
                    (true, Op::Ne) => W::F32Ne,
                    (true, Op::Lt) => W::F32Lt,
                    (true, Op::Le) => W::F32Le,
                    (true, Op::Gt) => W::F32Gt,
                    (true, Op::Ge) => W::F32Ge,
                    (false, Op::Eq) => W::F64Eq,
                    (false, Op::Ne) => W::F64Ne,
                    (false, Op::Lt) => W::F64Lt,
                    (false, Op::Le) => W::F64Le,
                    (false, Op::Gt) => W::F64Gt,
                    (false, Op::Ge) => W::F64Ge,
                    _ => unreachable!(),
                });
                if matches!(op, Op::Add | Op::Sub | Op::Mul | Op::Div) {
                    self.float_bits(p);
                } else {
                    self.ins(W::I64ExtendI32U);
                }
            }
        }
        Ok(())
    }
}
