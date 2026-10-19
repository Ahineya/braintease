//! Checked arithmetic used by collection capacity calculations.
use super::*;

impl<'a, 'tcx> FunctionCx<'a, 'tcx> {
    pub(super) fn checked_binary(
        &mut self,
        op: mir::BinOp,
        x: Datum<'tcx>,
        y: Datum<'tcx>,
        p: Address<'tcx>,
    ) -> Result<()> {
        let tcx = self.tcx();
        let signed = layout::signed(x.ty);
        let register = layout::carrier(tcx, x.ty)?.ok_or("non-scalar checked arithmetic")?;
        let operation = match op {
            mir::BinOp::AddWithOverflow => B::Add,
            mir::BinOp::SubWithOverflow => B::Sub,
            mir::BinOp::MulWithOverflow => B::Mul,
            _ => unreachable!(),
        };
        let mut value = mem::binary(
            &mut self.b,
            operation,
            x.value.clone(),
            y.value.clone(),
            register.clone(),
        )?;
        if layout::size(tcx, x.ty)? == 1 {
            value = mem::binary(
                &mut self.b,
                B::And,
                value,
                Value::Constant(255),
                register.clone(),
            )?;
            if signed {
                value = mem::binary(&mut self.b, B::Shl, value, Value::Constant(8), T::I16)?;
                value = mem::binary(&mut self.b, B::AShr, value, Value::Constant(8), T::I16)?;
            }
        }
        let flag = if signed {
            if operation == B::Mul {
                self.multiply_overflow(&x, &y, value.clone(), register, true)?
            } else {
                let first = mem::binary(
                    &mut self.b,
                    B::Xor,
                    x.value.clone(),
                    if operation == B::Add {
                        value.clone()
                    } else {
                        y.value.clone()
                    },
                    register.clone(),
                )?;
                let second = mem::binary(
                    &mut self.b,
                    B::Xor,
                    if operation == B::Add {
                        y.value.clone()
                    } else {
                        x.value.clone()
                    },
                    value.clone(),
                    register.clone(),
                )?;
                let bits = mem::binary(&mut self.b, B::And, first, second, register)?;
                mem::binary(&mut self.b, B::Slt, bits, Value::Constant(0), T::I16)?
            }
        } else {
            match operation {
                B::Add => mem::binary(&mut self.b, B::Ult, value.clone(), x.value, T::I16)?,
                B::Sub => mem::binary(&mut self.b, B::Ult, x.value, y.value, T::I16)?,
                B::Mul => self.multiply_overflow(&x, &y, value.clone(), register, false)?,
                _ => unreachable!(),
            }
        };
        mem::store(&mut self.b, tcx, p.pointer.clone(), value, x.ty)?;
        let offset = layout::layout(tcx, p.ty)?.fields.offset(1).bytes();
        let field = mem::offset(&mut self.b, p.pointer, Value::Constant(offset as i64))?;
        mem::store_bits(&mut self.b, field, flag, 1, T::I16)
    }

    fn multiply_overflow(
        &mut self,
        x: &Datum<'tcx>,
        y: &Datum<'tcx>,
        value: Value,
        register: T,
        signed: bool,
    ) -> Result<Value> {
        // Reversing the product detects overflow without a double-width
        // carrier. Neither division by zero nor signed MIN / -1 may execute.
        let nonzero = mem::binary(
            &mut self.b,
            B::Ne,
            y.value.clone(),
            Value::Constant(0),
            T::I16,
        )?;
        let yes = self.b.new_label();
        let no = self.b.new_label();
        let done = self.b.new_label();
        let flag = mem::alloca(&mut self.b, 1, 1)?;
        self.b.build_branch_cond(nonzero, yes, no)?;
        self.b.create_block(yes)?;
        if signed {
            let negative_one = mem::binary(
                &mut self.b,
                B::Eq,
                y.value.clone(),
                Value::Constant(-1),
                T::I16,
            )?;
            let special = self.b.new_label();
            let divide = self.b.new_label();
            self.b.build_branch_cond(negative_one, special, divide)?;
            self.b.create_block(special)?;
            let bits = layout::size(self.tcx(), x.ty)? * 8;
            let minimum = (1u64 << (bits - 1)) as i64;
            let minimum = if bits == 64 { minimum } else { -minimum };
            let overflow = mem::binary(
                &mut self.b,
                B::Eq,
                x.value.clone(),
                Value::Constant(minimum),
                T::I16,
            )?;
            mem::store_bits(&mut self.b, flag.clone(), overflow, 1, T::I16)?;
            self.b.build_branch(done)?;
            self.b.create_block(divide)?;
        }
        let quotient = mem::binary(
            &mut self.b,
            if signed { B::SDiv } else { B::UDiv },
            value,
            y.value.clone(),
            register,
        )?;
        let overflow = mem::binary(&mut self.b, B::Ne, quotient, x.value.clone(), T::I16)?;
        mem::store_bits(&mut self.b, flag.clone(), overflow, 1, T::I16)?;
        self.b.build_branch(done)?;
        self.b.create_block(no)?;
        mem::store_bits(&mut self.b, flag.clone(), Value::Constant(0), 1, T::I16)?;
        self.b.build_branch(done)?;
        self.b.create_block(done)?;
        mem::load_bits(&mut self.b, flag, 1, T::I16)
    }
}
