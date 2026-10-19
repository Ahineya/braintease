//! Match the compatibility backend's wrapping widths and defined div-by-zero.
use crate::emit::{width, Cg};
use rcc_ir::ir::{IrBinaryOp as Op, IrType, Value};
use wasm_encoder::{BlockType, Instruction as W};

impl Cg<'_> {
    pub fn binary(&mut self, op: Op, lhs: &Value, rhs: &Value, ty: &IrType) -> Result<(), String> {
        if ty.is_fp() {
            return Err("integer IR operations cannot produce floats; use float intrinsics".into());
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
                (Value::Constant(_), _) => self.width_of(rhs)?,
                (_, Value::Constant(_)) => self.width_of(lhs)?,
                _ => self.width_of(lhs)?.max(self.width_of(rhs)?),
            }
        } else {
            width(ty)?
        };
        let signed = matches!(
            op,
            Op::SDiv | Op::SRem | Op::Slt | Op::Sle | Op::Sgt | Op::Sge | Op::AShr
        );
        let a = self.scratch;
        let b = a + 1;
        self.value(lhs)?;
        self.mask(bits);
        if signed {
            self.signed(bits);
        }
        self.ins(W::LocalSet(a));
        self.value(rhs)?;
        self.mask(bits);
        if signed {
            self.signed(bits);
        }
        self.ins(W::LocalSet(b));
        if matches!(op, Op::SDiv | Op::SRem | Op::UDiv | Op::URem) {
            // Wasm traps on division by zero and signed MIN/-1. Ripple returns
            // zero for /0, and wraps MIN/-1. Guard before evaluating the divide.
            self.ins(W::LocalGet(b));
            self.ins(W::I64Eqz);
            self.ins(W::If(BlockType::Result(wasm_encoder::ValType::I64)));
            self.ins(W::I64Const(0));
            self.ins(W::Else);
            if signed && bits == 64 {
                self.ins(W::LocalGet(a));
                self.ins(W::I64Const(i64::MIN));
                self.ins(W::I64Eq);
                self.ins(W::LocalGet(b));
                self.ins(W::I64Const(-1));
                self.ins(W::I64Eq);
                self.ins(W::I32And);
                self.ins(W::If(BlockType::Result(wasm_encoder::ValType::I64)));
                self.ins(W::I64Const(if op == Op::SDiv { i64::MIN } else { 0 }));
                self.ins(W::Else);
            }
            self.ins(W::LocalGet(a));
            self.ins(W::LocalGet(b));
            self.ins(match op {
                Op::SDiv => W::I64DivS,
                Op::SRem => W::I64RemS,
                Op::UDiv => W::I64DivU,
                _ => W::I64RemU,
            });
            if signed && bits == 64 {
                self.ins(W::End);
            }
            self.ins(W::End);
        } else {
            self.ins(W::LocalGet(a));
            self.ins(W::LocalGet(b));
            if matches!(op, Op::Shl | Op::LShr | Op::AShr) {
                self.ins(W::I64Const(i64::from(bits - 1)));
                self.ins(W::I64And);
            }
            self.ins(match op {
                Op::Add => W::I64Add,
                Op::Sub => W::I64Sub,
                Op::Mul => W::I64Mul,
                Op::And => W::I64And,
                Op::Or => W::I64Or,
                Op::Xor => W::I64Xor,
                Op::Shl => W::I64Shl,
                Op::LShr => W::I64ShrU,
                Op::AShr => W::I64ShrS,
                Op::Eq => W::I64Eq,
                Op::Ne => W::I64Ne,
                Op::Slt => W::I64LtS,
                Op::Sle => W::I64LeS,
                Op::Sgt => W::I64GtS,
                Op::Sge => W::I64GeS,
                Op::Ult => W::I64LtU,
                Op::Ule => W::I64LeU,
                Op::Ugt => W::I64GtU,
                Op::Uge => W::I64GeU,
                _ => unreachable!(),
            });
            if comparison {
                self.ins(W::I64ExtendI32U);
            }
        }
        Ok(())
    }
}
