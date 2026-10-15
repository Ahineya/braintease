//! Native arithmetic and branches. No host callbacks or BF device knowledge.
use super::{
    region::{Action, Region},
    Native, State,
};
use cranelift_codegen::ir::{types, AbiParam, Block, InstBuilder, MemFlagsData, Value};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Switch};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{Linkage, Module};
use std::collections::BTreeMap;

pub(super) struct Compiler {
    module: Option<JITModule>,
    serial: u64,
}
impl Compiler {
    pub fn new() -> Result<Self, String> {
        let builder = JITBuilder::with_flags(
            &[("opt_level", "speed")],
            cranelift_module::default_libcall_names(),
        )
        .map_err(|e| format!("BF JIT initialization: {e}"))?;
        Ok(Self {
            module: Some(JITModule::new(builder)),
            serial: 0,
        })
    }
    pub fn compile(&mut self, region: &Region, fixed: bool) -> Result<(Native, usize), String> {
        let module = self.module.as_mut().unwrap();
        let mut context = module.make_context();
        let ptr_ty = module.target_config().pointer_type();
        context
            .func
            .signature
            .params
            .extend([AbiParam::new(ptr_ty), AbiParam::new(ptr_ty)]);
        let name = format!("bf_region_{}", self.serial);
        self.serial += 1;
        let id = module
            .declare_function(&name, Linkage::Local, &context.func.signature)
            .map_err(|e| e.to_string())?;
        let mut frontend = FunctionBuilderContext::new();
        {
            let mut b = FunctionBuilder::new(&mut context.func, &mut frontend);
            let entry = b.create_block();
            b.append_block_params_for_function_params(entry);
            let blocks: BTreeMap<_, _> = region
                .nodes
                .keys()
                .map(|&pc| (pc, b.create_block()))
                .collect();
            let end = b.create_block();
            for _ in 0..3 {
                b.append_block_param(end, types::I64);
            }
            for &block in blocks.values() {
                b.append_block_param(block, types::I64); // dynamic pointer
                b.append_block_param(block, types::I64); // remaining budget
            }
            b.switch_to_block(entry);
            let tape = b.block_params(entry)[0];
            let state = b.block_params(entry)[1];
            let pc = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, pc) as i32,
            );
            let pointer = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, pointer) as i32,
            );
            let budget = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, remaining) as i32,
            );
            let low = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, low) as i32,
            );
            let high = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, high) as i32,
            );
            let length = b.ins().load(
                types::I64,
                MemFlagsData::trusted(),
                state,
                std::mem::offset_of!(State, length) as i32,
            );
            let miss = b.create_block();
            let mut switch = Switch::new();
            let entries: Vec<_> = blocks
                .iter()
                .map(|(&pc, &block)| (pc, block, b.create_block()))
                .collect();
            for &(pc, _, trampoline) in &entries {
                switch.set_entry(pc as u128, trampoline);
            }
            switch.emit(&mut b, pc, miss);
            for &(_, block, trampoline) in &entries {
                b.switch_to_block(trampoline);
                b.ins().jump(block, &[pointer.into(), budget.into()]);
            }
            b.switch_to_block(miss);
            b.ins()
                .jump(end, &[pc.into(), pointer.into(), budget.into()]);
            for (&pc, node) in &region.nodes {
                b.switch_to_block(blocks[&pc]);
                let pointer = b.block_params(blocks[&pc])[0];
                let budget = b.block_params(blocks[&pc])[1];
                let body = b.create_block();
                let exhausted =
                    b.ins()
                        .icmp_imm_s(cranelift_codegen::ir::condcodes::IntCC::Equal, budget, 0);
                let exit_pc = b.ins().iconst(types::I64, pc as i64);
                b.ins().brif(
                    exhausted,
                    end,
                    &[exit_pc.into(), pointer.into(), budget.into()],
                    body,
                    &[],
                );
                b.switch_to_block(body);
                if !fixed {
                    let minimum = if matches!(node.action, Action::Move { .. }) {
                        b.ins().iconst(types::I64, 0)
                    } else {
                        low
                    };
                    let maximum = if matches!(node.action, Action::Move { .. }) {
                        length
                    } else {
                        high
                    };
                    let first = b.ins().iadd_imm_s(minimum, -(node.minimum as i64));
                    let last = b.ins().iadd_imm_s(pointer, node.maximum as i64);
                    let below = b.ins().icmp(
                        cranelift_codegen::ir::condcodes::IntCC::UnsignedLessThan,
                        pointer,
                        first,
                    );
                    let above = b.ins().icmp(
                        cranelift_codegen::ir::condcodes::IntCC::UnsignedGreaterThanOrEqual,
                        last,
                        maximum,
                    );
                    let outside = b.ins().bor(below, above);
                    let valid = b.create_block();
                    b.ins().brif(
                        outside,
                        end,
                        &[exit_pc.into(), pointer.into(), budget.into()],
                        valid,
                        &[],
                    );
                    b.switch_to_block(valid);
                }
                let remaining = b.ins().iadd_imm_s(budget, -1);
                let scaled = b.ins().ishl_imm_u(pointer, 1);
                let current = b.ins().iadd(tape, scaled);
                let after = b.ins().iadd_imm_s(pointer, node.after as i64);
                match &node.action {
                    Action::Arithmetic {
                        changes,
                        inverse,
                        next,
                    } => {
                        let multiplier = if let Some(inverse) = inverse {
                            let control = load(&mut b, current, 0);
                            let neg = b.ins().ineg(control);
                            Some(b.ins().imul_imm_u(neg, i64::from(*inverse)))
                        } else {
                            None
                        };
                        for &(at, delta) in changes {
                            let old = load(&mut b, current, at);
                            let value = if let Some(multiplier) = multiplier {
                                let amount = b.ins().imul_imm_u(multiplier, i64::from(delta));
                                b.ins().iadd(old, amount)
                            } else {
                                b.ins().iadd_imm_s(old, i64::from(delta))
                            };
                            store(&mut b, current, at, value);
                        }
                        if inverse.is_some() {
                            let zero = b.ins().iconst(types::I16, 0);
                            store(&mut b, current, 0, zero);
                        }
                        edge(&mut b, &blocks, end, *next, after, remaining);
                    }
                    Action::Move { next } => edge(&mut b, &blocks, end, *next, after, remaining),
                    Action::Branch { zero, nonzero } => {
                        let value = load(&mut b, current, 0);
                        let predicate = b.ins().icmp_imm_s(
                            cranelift_codegen::ir::condcodes::IntCC::Equal,
                            value,
                            0,
                        );
                        let yes = b.create_block();
                        let no = b.create_block();
                        b.ins().brif(predicate, yes, &[], no, &[]);
                        b.switch_to_block(yes);
                        edge(&mut b, &blocks, end, *zero, pointer, remaining);
                        b.switch_to_block(no);
                        edge(&mut b, &blocks, end, *nonzero, pointer, remaining);
                    }
                }
            }
            b.switch_to_block(end);
            let values = b.block_params(end).to_vec();
            for (offset, value) in [
                std::mem::offset_of!(State, pc),
                std::mem::offset_of!(State, pointer),
                std::mem::offset_of!(State, remaining),
            ]
            .into_iter()
            .zip(values)
            {
                b.ins()
                    .store(MemFlagsData::trusted(), value, state, offset as i32);
            }
            b.ins().return_(&[]);
            b.seal_all_blocks();
            b.finalize(module.target_config());
        }
        module
            .define_function(id, &mut context)
            .map_err(|e| format!("BF JIT compile: {e}"))?;
        let bytes = context.compiled_code().map_or(0, |c| c.code_buffer().len());
        module
            .finalize_definitions()
            .map_err(|e| format!("BF JIT finalize: {e}"))?;
        let pointer = module.get_finalized_function(id);
        // SAFETY: the signature is (two native pointers) -> void, and all generated
        // accesses are guarded before invocation. Code lives until Compiler drops.
        Ok((
            unsafe { std::mem::transmute::<*const u8, Native>(pointer) },
            bytes,
        ))
    }
}
impl Drop for Compiler {
    fn drop(&mut self) {
        if let Some(module) = self.module.take() {
            // SAFETY: native calls are synchronous, with no callbacks. Dropping a
            // compiler is possible only after all its function pointers returned.
            unsafe {
                module.free_memory();
            }
        }
    }
}
fn address(b: &mut FunctionBuilder<'_>, tape: Value, offset: isize) -> Value {
    b.ins().iadd_imm_s(tape, (offset as i64) * 2)
}
fn load(b: &mut FunctionBuilder<'_>, tape: Value, offset: isize) -> Value {
    let ptr = address(b, tape, offset);
    b.ins().load(types::I16, MemFlagsData::trusted(), ptr, 0)
}
fn store(b: &mut FunctionBuilder<'_>, tape: Value, offset: isize, value: Value) {
    let ptr = address(b, tape, offset);
    b.ins().store(MemFlagsData::trusted(), value, ptr, 0);
}
fn edge(
    b: &mut FunctionBuilder<'_>,
    blocks: &BTreeMap<usize, Block>,
    end: Block,
    pc: usize,
    pointer: Value,
    budget: Value,
) {
    if let Some(&block) = blocks.get(&pc) {
        b.ins().jump(block, &[pointer.into(), budget.into()]);
    } else {
        let pc = b.ins().iconst(types::I64, pc as i64);
        b.ins()
            .jump(end, &[pc.into(), pointer.into(), budget.into()]);
    }
}
