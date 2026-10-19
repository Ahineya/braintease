//! Direct calls and checked code-token/table calls share the private scalar ABI.
use crate::emit::Cg;
use rcc_ir::ir::{Instruction as I, IrType, Value};
use wasm_encoder::Instruction as W;
impl Cg<'_> {
    pub fn call(&mut self, inst: &I) -> Result<(), String> {
        let I::Call {
            result,
            function,
            args,
            result_type,
            stack_args_from,
        } = inst
        else {
            unreachable!()
        };
        if stack_args_from.is_some() {
            return Err("Wasm variadic calls are not implemented yet".into());
        }
        let direct = if let Value::Function(n) | Value::Global(n) = function {
            let (index, callee) = self
                .context
                .functions
                .get(n)
                .ok_or_else(|| format!("Wasm external function {n} has no runtime binding"))?;
            let same = callee.return_type == *result_type
                || (callee.return_type.is_pointer() && result_type.is_pointer());
            if args.len() != callee.parameters.len()
                || (!same && (result.is_some() || *result_type != IrType::Void))
            {
                return Err(format!("incompatible Wasm call signature for {n}"));
            }
            Some(*index)
        } else {
            None
        };
        for arg in args {
            self.value(arg)?;
        }
        if let Some(index) = direct {
            self.ins(W::Call(index));
        } else {
            self.value(function)?;
            self.ins(W::LocalTee(self.scratch));
            self.ins(W::I64Const(0x8000_0000));
            self.ins(W::I64LtU);
            self.ins(W::LocalGet(self.scratch));
            self.ins(W::I64Const(0xffff_ffff));
            self.ins(W::I64GtU);
            self.ins(W::I32Or);
            self.trap_if(6);
            self.ins(W::LocalGet(self.scratch));
            self.ins(W::I64Const(0x7fff_ffff));
            self.ins(W::I64And);
            self.ins(W::I32WrapI64);
            let signature = *self
                .context
                .signatures
                .get(&args.len())
                .ok_or("unknown Wasm indirect call arity")?;
            self.ins(W::I32Const(6));
            self.ins(W::GlobalSet(crate::memory::FAULT));
            self.ins(W::CallIndirect {
                type_index: signature,
                table_index: 0,
            });
            self.ins(W::I32Const(0));
            self.ins(W::GlobalSet(crate::memory::FAULT));
        }
        if let Some(result) = result {
            self.define(*result)?;
        } else {
            self.ins(W::Drop);
        }
        Ok(())
    }
}
