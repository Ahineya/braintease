//! Memory and frame instruction selection. No memory operation calls JS.
use crate::{
    emit::{width, Cg},
    memory::{self, BANK},
};
use rcc_ir::{
    ir::{Instruction as I, Value},
    target::Target,
};
use wasm_encoder::{BlockType, Instruction as W};
impl Cg<'_> {
    pub fn trap_if(&mut self, code: i32) {
        self.ins(W::If(BlockType::Empty));
        memory::fault(&mut self.body, code);
        self.ins(W::End);
    }
    pub fn prepare_frame(&mut self) -> Result<(), String> {
        let mut cells = 0u64;
        for inst in self.function.blocks.iter().flat_map(|b| &b.instructions) {
            if let I::Alloca {
                result,
                alloc_type,
                count,
                ..
            } = inst
            {
                let count = match count {
                    None => 1,
                    Some(Value::Constant(n)) if *n >= 0 => *n as u64,
                    _ => return Err("Wasm requires constant alloca sizes".into()),
                };
                self.allocas.insert(*result, cells);
                cells = cells
                    .checked_add(
                        Target::Wasm
                            .layout()
                            .size_in_cells(alloc_type)?
                            .checked_mul(count)
                            .ok_or("Wasm alloca overflow")?,
                    )
                    .ok_or("Wasm frame overflow")?;
            }
        }
        if cells >= 3 * BANK {
            return Err("Wasm compatibility stack frame exceeds three banks".into());
        }
        self.ins(W::GlobalGet(memory::STACK));
        self.ins(W::LocalSet(self.mark));
        self.ins(W::LocalGet(self.mark));
        self.ins(W::I64Const(cells as i64));
        self.ins(W::I64Add);
        self.ins(W::LocalTee(self.scratch));
        self.ins(W::I64Const((5 * BANK) as i64));
        self.ins(W::I64GeU);
        self.trap_if(3);
        self.ins(W::LocalGet(self.scratch));
        self.ins(W::GlobalSet(memory::STACK));
        self.ins(W::LocalGet(self.mark));
        self.ins(W::I64Const(BANK as i64));
        self.ins(W::I64DivU);
        self.ins(W::I64Const(16));
        self.ins(W::I64Shl);
        self.ins(W::LocalGet(self.mark));
        self.ins(W::I64Const(BANK as i64));
        self.ins(W::I64RemU);
        self.ins(W::I64Or);
        self.ins(W::LocalSet(self.frame));
        Ok(())
    }
    pub fn memory(&mut self, inst: &I) -> Result<(), String> {
        match inst {
            I::Load {
                result,
                ptr,
                result_type,
                ..
            } => {
                let bits = width(result_type)?;
                let cells = Target::Wasm.layout().size_in_cells(result_type)?;
                self.value(ptr)?;
                self.ins(W::I64Const(cells as i64));
                self.ins(W::Call(self.context.address));
                self.ins(W::I32WrapI64);
                self.ins(match cells {
                    1 => W::I64Load16U(memory::memarg()),
                    2 => W::I64Load32U(memory::memarg()),
                    4 => W::I64Load(memory::memarg()),
                    _ => return Err("Wasm aggregate loads require scalarization".into()),
                });
                self.mask(bits);
                self.define(*result)?;
            }
            I::Store {
                value,
                ptr,
                value_type,
                ..
            } => {
                let bits = width(value_type)?;
                let cells = Target::Wasm.layout().size_in_cells(value_type)?;
                self.value(ptr)?;
                self.ins(W::I64Const(cells as i64));
                self.ins(W::Call(self.context.address));
                self.ins(W::I32WrapI64);
                self.value(value)?;
                self.mask(bits);
                self.ins(match cells {
                    1 => W::I64Store16(memory::memarg()),
                    2 => W::I64Store32(memory::memarg()),
                    4 => W::I64Store(memory::memarg()),
                    _ => return Err("Wasm aggregate stores require scalarization".into()),
                });
            }
            I::Alloca { result, .. } => {
                self.ins(W::LocalGet(self.frame));
                self.ins(W::I64Const(self.allocas[result] as i64));
                self.ins(W::Call(self.context.gep));
                self.define(*result)?;
            }
            I::GetElementPtr {
                result,
                ptr,
                indices,
                source_element_type,
                is_struct_field,
                ..
            } => {
                if indices.len() != 1 {
                    return Err("Wasm requires a single GEP index".into());
                }
                let stride = if *is_struct_field {
                    1
                } else {
                    Target::Wasm.layout().size_in_cells(source_element_type)?
                };
                if stride > memory::CELLS {
                    return Err("Wasm GEP element exceeds memory".into());
                }
                self.value(ptr)?;
                self.value(&indices[0])?;
                if !matches!(indices[0], Value::Constant(_)) {
                    let bits = self.width_of(&indices[0])?;
                    if bits <= 16 {
                        self.mask(bits);
                    } else {
                        self.signed(bits);
                    }
                }
                self.ins(W::LocalSet(self.scratch));
                if stride != 0 {
                    let limit = (memory::CELLS / stride) as i64;
                    self.ins(W::LocalGet(self.scratch));
                    self.ins(W::I64Const(-limit));
                    self.ins(W::I64LtS);
                    self.ins(W::LocalGet(self.scratch));
                    self.ins(W::I64Const(limit));
                    self.ins(W::I64GtS);
                    self.ins(W::I32Or);
                    self.trap_if(2);
                }
                self.ins(W::LocalGet(self.scratch));
                self.ins(W::I64Const(stride as i64));
                self.ins(W::I64Mul);
                self.ins(W::Call(self.context.gep));
                self.define(*result)?;
            }
            _ => unreachable!(),
        }
        Ok(())
    }
}
