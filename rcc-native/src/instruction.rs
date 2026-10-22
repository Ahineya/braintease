//! Scalar instruction selection; control-flow edge construction lives in emit.
use super::emit::{width, Cg};
use super::*;
use cranelift_codegen::ir::{condcodes::IntCC, InstBuilder};
use rcc_ir::ir::{Instruction as I, IrType, IrUnaryOp, Value};

impl Cg<'_, '_> {
    pub(super) fn instruction(&mut self, instruction: &I, block: u32) -> Result<(), String> {
        match instruction {
            I::Binary { result, op, lhs, rhs, result_type } => {
                let value = self.binary(*op, lhs, rhs, result_type)?;
                self.define(*result, value)?;
            }
            I::Unary { result, op, operand, result_type } => {
                let value = self.value(operand)?;
                let source_bits = self.width_of(operand);
                let value = match op {
                    IrUnaryOp::Not => self.b.ins().bnot(value),
                    IrUnaryOp::Neg => self.b.ins().ineg(value),
                    IrUnaryOp::SExt => self.signed(value, source_bits),
                    IrUnaryOp::ZExt => self.mask(value, source_bits),
                    IrUnaryOp::Trunc | IrUnaryOp::PtrToInt | IrUnaryOp::IntToPtr => value,
                    IrUnaryOp::PtrToLinear => {
                        let bank = self.b.ins().ushr_imm_u(value, 16);
                        let bank = self.b.ins().band_imm_u(bank, 65535);
                        let bank = self.b.ins().imul_imm_s(bank, i64::from(Target::Native.layout().bank_cells));
                        let addr = self.b.ins().band_imm_u(value, 65535);
                        self.b.ins().iadd(bank, addr)
                    }
                };
                let value = self.mask(value, width(result_type));
                self.define(*result, value)?;
            }
            I::Load { result, ptr, result_type, volatile } => {
                if matches!(result_type, IrType::Array { .. } | IrType::Struct { .. }) {
                    return Err("native aggregate load requires scalarization".into());
                }
                let ptr = self.value(ptr)?;
                let words = Target::Native.layout().size_in_cells(result_type)?;
                let value = self.scalar_load(ptr, words, *volatile)?;
                let value = self.mask(value, width(result_type));
                self.define(*result, value)?;
            }
            I::Store { value, ptr, value_type, .. } => {
                if matches!(value_type, IrType::Array { .. } | IrType::Struct { .. }) {
                    return Err("native aggregate store requires scalarization".into());
                }
                let ptr = self.value(ptr)?;
                let value = self.value(value)?;
                let words = self.k(Target::Native.layout().size_in_cells(value_type)? as i64);
                self.runtime("rcc_native_store", &[ptr, value, words])?;
            }
            I::GetElementPtr { result, ptr, indices, source_element_type, is_struct_field, .. } => {
                if indices.len() != 1 {
                    return Err("native backend requires a single GEP index".into());
                }
                let pointer = self.value(ptr)?;
                let index = self.value(&indices[0])?;
                let bits = self.width_of(&indices[0]);
                let index = if matches!(indices[0], Value::Constant(_)) {
                    index
                } else if bits <= 16 {
                    // Shared GEP narrowing removes ZExt to I32 for native-word
                    // indices. These retain unsigned semantics; signed C indices
                    // keep their explicit SExt to I32 in the IR.
                    self.mask(index, bits)
                } else {
                    self.signed(index, bits)
                };
                let size = if *is_struct_field { 1 } else {
                    Target::Native.layout().size_in_cells(source_element_type)?
                };
                let offset = self.b.ins().imul_imm_s(index, i64::try_from(size).map_err(|_| "GEP size overflow")?);
                let value = self.logical_gep(pointer, offset)?;
                self.define(*result, value)?;
            }
            I::Alloca { result, .. } => {
                let offset = self.k(self.allocas[result] as i64);
                let value = self.logical_gep(self.frame, offset)?;
                self.define(*result, value)?;
            }
            I::Call { result, function, args, result_type, stack_args_from } => {
                let mut values = vec![self.context];
                for arg in args { values.push(self.value(arg)?); }
                if let Some(fixed) = stack_args_from {
                    let offset = self.k(self.var_area as i64);
                    let base = self.logical_gep(self.frame, offset)?;
                    let mut cursor = base;
                    for (i,arg) in args.iter().enumerate().skip(*fixed) {
                        let words = super::emit::argument_words(arg,&self.types);
                        let count = self.k(words as i64);
                        self.runtime("rcc_native_store", &[cursor,values[i+1],count])?;
                        cursor = self.logical_gep(cursor, count)?;
                    }
                    values.truncate(fixed+1);
                    values.push(base);
                }
                let call = if let Value::Global(name) | Value::Function(name) = function {
                    let id = self.compiler.callee(name, args.len())?;
                    let reference = self.compiler.object.declare_func_in_func(id, self.b.func);
                    self.b.ins().call(reference, &values)
                } else {
                    let code = self.value(function)?;
                    let count = self.k((values.len()-1) as i64);
                    let address = self.runtime("rcc_native_function", &[code, count])?;
                    let sig = self.b.import_signature(self.compiler.signature(values.len()-1));
                    self.b.ins().call_indirect(sig, address, &values)
                };
                if let Some(result) = result {
                    let value = self.b.inst_results(call)[0];
                    let value = self.mask(value, width(result_type));
                    self.define(*result, value)?;
                }
            }
            I::Return(value) => {
                let value = if let Some(value) = value { self.value(value)? } else { self.k(0) };
                if self.owns_stack {
                    self.runtime("rcc_native_stack_restore", &[self.mark])?;
                }
                self.b.ins().return_(&[value]);
            }
            I::Branch(to) => {
                self.runtime("rcc_native_poll", &[])?;
                let target = self.edge(block, *to)?;
                self.b.ins().jump(target, &[]);
            }
            I::BranchCond { condition, true_label, false_label } => {
                if let Some(plan) = self.dispatches.get(&block).cloned() {
                    self.dispatch(&plan)?;
                }
                self.runtime("rcc_native_poll", &[])?;
                let condition = self.value(condition)?;
                let condition = self.b.ins().icmp_imm_s(IntCC::NotEqual, condition, 0);
                let t = self.edge(block, *true_label)?;
                let f = self.edge(block, *false_label)?;
                self.b.ins().brif(condition, t, &[], f, &[]);
            }
            I::Phi { .. } => {},
            I::Select { result, condition, true_value, false_value, result_type } => {
                let cond = self.value(condition)?;
                let cond = self.b.ins().icmp_imm_s(IntCC::NotEqual, cond, 0);
                let t = self.value(true_value)?;
                let f = self.value(false_value)?;
                let value = self.b.ins().select(cond, t, f);
                let value = self.mask(value, width(result_type));
                self.define(*result, value)?;
            }
            I::Cast { result, value, target_type } => {
                let value = self.value(value)?;
                let value = self.mask(value, width(target_type));
                self.define(*result, value)?;
            }
            I::Intrinsic { intrinsic, args, result_type, result } if intrinsic == "va_start" => {
                if args.len()!=1 || result.is_some() || *result_type!=IrType::Void { return Err("invalid va_start signature".into()); }
                let input = self.va_input.ok_or("va_start outside a variadic function")?;
                let ap = self.value(&args[0])?;
                let words = self.k(2);
                self.runtime("rcc_native_store", &[ap,input,words])?;
            }
            I::Intrinsic { intrinsic, args, result_type, result } if intrinsic == "va_arg" => {
                if args.len()!=1 { return Err("invalid va_arg signature".into()); }
                let ap = self.value(&args[0])?;
                let pointer_words = self.k(2);
                let pointer = self.runtime("rcc_native_load", &[ap,pointer_words])?;
                let words = self.k(Target::Native.layout().size_in_cells(result_type)? as i64);
                let value = self.runtime("rcc_native_load", &[pointer,words])?;
                let next = self.logical_gep(pointer, words)?;
                self.runtime("rcc_native_store", &[ap,next,pointer_words])?;
                self.define(result.ok_or("va_arg requires a result")?,value)?;
            }
            I::Intrinsic { result, intrinsic, args, result_type } => {
                let value = self.floating(intrinsic, args, result_type)?;
                let result = result.ok_or("native floating intrinsic must return a value")?;
                self.define(result, value)?;
            }
            I::Comment(_) | I::DebugLoc { .. } => {},
            I::InlineAsm { .. } => return Err("Ripple inline assembly is unavailable on the native target; use a target-gated runtime binding".into()),
        }
        Ok(())
    }
}
