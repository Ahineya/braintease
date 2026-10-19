//! Opt-in stackless native continuations. Calls and loop checkpoints split functions; ordinary
//! arithmetic and CFGs still compile to host instructions. Only live SSA values
//! cross a suspension; immutable parameters are recovered from frame arguments.
use super::emit::{argument_words, result_type, width, Cg};
use super::*;
use cranelift_codegen::ir::{InstBuilder, Value as ClValue};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Switch};
use rcc_ir::ir::{Function, Instruction as I, Value};

fn operation(name: &str) -> Option<(u64, usize)> {
    Some(match name {
        "rcc_process_fork" => (1, 0),
        "rcc_process_yield" => (2, 0),
        "rcc_process_wait" => (3, 2),
        "rcc_process_exit" | "exit" => (4, 1),
        "rcc_process_getpid" => (5, 0),
        "rcc_process_kernel_alloc" => (6, 1),
        "rcc_process_kernel_free" => (7, 1),
        "rcc_hosted_clone" => (8, 1),
        "rcc_hosted_switch" => (9, 1),
        "rcc_hosted_drop_switch" => (10, 1),
        "rcc_hosted_exec" => (11, 2),
        "rcc_hosted_syscall" => (12, 1),
        "setjmp" => (13, 1),
        "longjmp" => (14, 2),
        _ => return None,
    })
}

pub(super) fn enabled(module: &Module) -> bool {
    module
        .functions
        .iter()
        .flat_map(|f| &f.blocks)
        .flat_map(|b| &b.instructions)
        .any(|i| {
            matches!(i, I::Call {
            function: Value::Global(n) | Value::Function(n), ..
        } if (n.starts_with("rcc_process_") || n.starts_with("rcc_hosted_")) && operation(n).is_some())
        })
}

pub(super) fn hosted(module: &Module) -> bool {
    module
        .functions
        .iter()
        .flat_map(|f| &f.blocks)
        .flat_map(|b| &b.instructions)
        .any(|i| {
            matches!(i, I::Call {function: Value::Global(n) | Value::Function(n), ..}
            if n.starts_with("rcc_hosted_") && operation(n).is_some())
        })
}

fn continuation_slots(
    live: &BTreeSet<u32>,
    parameters: &BTreeMap<u32, usize>,
) -> BTreeMap<u32, u64> {
    live.iter()
        .filter(|id| !parameters.contains_key(id))
        .enumerate()
        .map(|(index, id)| (*id, index as u64))
        .collect()
}

impl Cg<'_, '_> {
    fn reload_continuation(
        &mut self,
        live: &BTreeSet<u32>,
        parameters: &BTreeMap<u32, usize>,
    ) -> Result<(), String> {
        let slots = continuation_slots(live, parameters);
        for id in live {
            let value = if let Some(index) = parameters.get(id) {
                let index = self.k(*index as i64);
                self.runtime("rcc_native_process_arg", &[index])?
            } else {
                let slot = self.k(slots[id] as i64);
                self.runtime("rcc_native_process_slot", &[slot])?
            };
            let bits = width(
                self.types
                    .get(id)
                    .ok_or_else(|| format!("undefined continuation temporary %{id}"))?,
            );
            let value = self.mask(value, bits);
            self.define(*id, value)?;
        }
        Ok(())
    }

    fn checkpoint(
        &mut self,
        pc: u64,
        live: &BTreeSet<u32>,
        parameters: &BTreeMap<u32, usize>,
    ) -> Result<(cranelift_codegen::ir::Block, cranelift_codegen::ir::Block), String> {
        let due = self.runtime("rcc_native_process_checkpoint", &[])?;
        let suspend = self.b.create_block();
        let resume = self.b.create_block();
        let proceed = self.b.create_block();
        self.b.ins().brif(due, suspend, &[], proceed, &[]);
        self.b.switch_to_block(suspend);
        let slots = continuation_slots(live, parameters);
        self.save_continuation(pc, &slots, None)?;
        let op = self.k(15);
        let zero = self.k(0);
        self.runtime("rcc_native_process_request", &[op, zero, zero, zero])?;
        self.b.ins().return_(&[zero]);
        self.b.switch_to_block(resume);
        self.reload_continuation(live, parameters)?;
        self.b.ins().jump(proceed, &[]);
        self.b.switch_to_block(proceed);
        Ok((resume, proceed))
    }

    fn save_continuation(
        &mut self,
        pc: u64,
        slots: &BTreeMap<u32, u64>,
        skip: Option<u32>,
    ) -> Result<(), String> {
        let pc = self.k(pc as i64);
        // Clear slots from the previous suspension before storing this one.
        self.runtime("rcc_native_process_set_pc", &[pc])?;
        for (id, index) in slots {
            if Some(*id) == skip {
                continue;
            }
            let var = *self
                .vars
                .get(id)
                .ok_or_else(|| format!("undefined continuation temporary %{id}"))?;
            let value = self.b.use_var(var);
            let slot = self.k(*index as i64);
            self.runtime("rcc_native_process_save", &[slot, value])?;
        }
        Ok(())
    }

    fn suspend_call(
        &mut self,
        instruction: &I,
        pc: u64,
        live: &BTreeSet<u32>,
        parameters: &BTreeMap<u32, usize>,
    ) -> Result<(), String> {
        let I::Call {
            function,
            args,
            result,
            stack_args_from,
            ..
        } = instruction
        else {
            unreachable!()
        };
        let mut values = args
            .iter()
            .map(|v| self.value(v))
            .collect::<Result<Vec<_>, _>>()?;
        if let Some(fixed) = stack_args_from {
            if *fixed > args.len() {
                return Err("invalid resumable variadic call".into());
            }
            let offset = self.k(self.var_area as i64);
            let base = self.runtime("rcc_native_gep", &[self.frame, offset])?;
            let mut cursor = base;
            for (index, arg) in args.iter().enumerate().skip(*fixed) {
                let count = self.k(argument_words(arg, &self.types) as i64);
                self.runtime("rcc_native_store", &[cursor, values[index], count])?;
                cursor = self.runtime("rcc_native_gep", &[cursor, count])?;
            }
            values.truncate(*fixed);
            values.push(base);
        }
        let slots = continuation_slots(live, parameters);
        self.save_continuation(pc, &slots, *result)?;
        // Zero discards the result; compact slot zero is encoded as one.
        let slot = self.k(result
            .and_then(|id| slots.get(&id))
            .map_or(0, |index| *index as i64 + 1));
        let op = match function {
            Value::Global(n) | Value::Function(n)
                if self
                    .compiler
                    .ir
                    .get_function(n)
                    .is_none_or(|f| f.is_external) =>
            {
                operation(n)
            }
            _ => None,
        };
        if let Some((op, arity)) = op {
            if args.len() != arity {
                return Err("native process service argument count mismatch".into());
            }
            let op = self.k(op as i64);
            let zero = self.k(0);
            self.runtime(
                "rcc_native_process_request",
                &[
                    op,
                    values.first().copied().unwrap_or(zero),
                    values.get(1).copied().unwrap_or(zero),
                    slot,
                ],
            )?;
        } else {
            let token = self.value(function)?;
            for (index, value) in values.iter().enumerate() {
                let index = self.k(index as i64);
                self.runtime("rcc_native_process_argument", &[index, *value])?;
            }
            let count = self.k(values.len() as i64);
            self.runtime("rcc_native_process_call", &[token, count, slot])?;
        }
        let zero = self.k(0);
        self.b.ins().return_(&[zero]);
        Ok(())
    }
}

fn suspends(compiler: &Compiler, instruction: &I) -> bool {
    let I::Call { function, .. } = instruction else {
        return false;
    };
    match function {
        Value::Global(n) | Value::Function(n) => {
            !compiler.direct_functions.contains_key(n)
                && (operation(n).is_some()
                    || compiler.ir.get_function(n).is_some_and(|f| !f.is_external))
        }
        _ => true,
    }
}

pub(super) fn emit_function(compiler: &mut Compiler, function: &Function) -> Result<(), String> {
    // Hosted kernel units remain uninterrupted. Independently loaded user code
    // and standalone process programs provide bounded scheduling opportunities.
    let checkpoints = compiler.application || !hosted(&compiler.ir);
    let liveness = super::liveness::Liveness::analyze(function)?;
    let parameters: BTreeMap<_, _> = function
        .parameters
        .iter()
        .enumerate()
        .map(|(index, (id, _))| (*id, index))
        .collect();
    let mut context = compiler.object.make_context();
    context.func.signature = compiler.signature(0);
    let mut frontend = FunctionBuilderContext::new();
    {
        let mut b = FunctionBuilder::new(&mut context.func, &mut frontend);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        let runtime = b.block_params(entry)[0];
        let mut types_map: BTreeMap<_, _> = function.parameters.iter().cloned().collect();
        let mut allocas = BTreeMap::new();
        let mut cells = 0u64;
        let mut phis = BTreeMap::new();
        for block in &function.blocks {
            if !block.has_terminator() {
                return Err(format!(
                    "native function {} block {} has no terminator",
                    function.name, block.id
                ));
            }
            for inst in &block.instructions {
                if let Some((id, ty)) = result_type(inst) {
                    types_map.insert(id, ty);
                }
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
                        _ => {
                            return Err(
                                "resumable native code requires constant alloca sizes".into()
                            )
                        }
                    };
                    allocas.insert(*result, cells);
                    cells = cells
                        .checked_add(
                            Target::Native
                                .layout()
                                .size_in_cells(alloc_type)?
                                .checked_mul(count)
                                .ok_or("native local size overflow")?,
                        )
                        .ok_or("native frame overflow")?;
                }
                if let I::Phi {
                    result, incoming, ..
                } = inst
                {
                    phis.entry(block.id)
                        .or_insert_with(Vec::new)
                        .push((*result, incoming.clone()));
                }
            }
        }
        let var_area = cells;
        let mut extra = 0;
        for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
            if let I::Call {
                args,
                stack_args_from: Some(fixed),
                ..
            } = inst
            {
                if *fixed > args.len() {
                    return Err("invalid variadic call".into());
                }
                extra = extra.max(
                    args[*fixed..]
                        .iter()
                        .map(|a| argument_words(a, &types_map))
                        .sum::<u64>(),
                );
            }
        }
        cells = cells.checked_add(extra).ok_or("native frame overflow")?;
        let vars = types_map
            .keys()
            .map(|&id| (id, b.declare_var(types::I64)))
            .collect();
        let blocks = function
            .blocks
            .iter()
            .map(|block| (block.id, b.create_block()))
            .collect();
        let zero = b.ins().iconst(types::I64, 0);
        let mut cg = Cg {
            b,
            compiler,
            context: runtime,
            vars,
            types: types_map,
            blocks,
            phis,
            frame: zero,
            mark: zero,
            owns_stack: false,
            allocas,
            edges: Vec::new(),
            va_input: None,
            var_area,
        };
        let size = cg.k(i64::try_from(cells).map_err(|_| "native frame size overflow")?);
        cg.frame = cg.runtime("rcc_native_process_frame", &[size])?;
        for id in cg.vars.keys().copied().collect::<Vec<_>>() {
            cg.define(id, zero)?;
        }
        if function.is_vararg {
            let index = cg.k(function.parameters.len() as i64);
            cg.va_input = Some(cg.runtime("rcc_native_process_arg", &[index])?);
        }
        let dispatch = cg.b.create_block();
        cg.b.ins().jump(dispatch, &[]);
        let mut switch = Switch::new();
        let first = function
            .blocks
            .first()
            .ok_or("resumable function has no body")?;
        let initial = cg.b.create_block();
        switch.set_entry(0, initial);
        cg.b.switch_to_block(initial);
        for (id, index) in &parameters {
            let index = cg.k(*index as i64);
            let value = cg.runtime("rcc_native_process_arg", &[index])?;
            let value = cg.mask(value, width(&cg.types[id]));
            cg.define(*id, value)?;
        }
        cg.b.ins().jump(cg.blocks[&first.id], &[]);
        let mut pc = 0u64;
        for (block_index, block) in function.blocks.iter().enumerate() {
            cg.b.switch_to_block(cg.blocks[&block.id]);
            for (position, instruction) in block.instructions.iter().enumerate() {
                let backward = |target: &u32| {
                    function.blocks[..=block_index]
                        .iter()
                        .any(|b| b.id == *target)
                };
                let loop_edge = match instruction {
                    I::Branch(to) => backward(to),
                    I::BranchCond {
                        true_label,
                        false_label,
                        ..
                    } => backward(true_label) || backward(false_label),
                    _ => false,
                };
                if checkpoints && loop_edge {
                    pc += 1;
                    let live = liveness.before(block_index, position, instruction);
                    let (resume, _) = cg.checkpoint(pc, &live, &parameters)?;
                    switch.set_entry(u128::from(pc), resume);
                }
                if suspends(cg.compiler, instruction) {
                    pc += 1;
                    let live = &liveness.after[block_index][position];
                    cg.suspend_call(instruction, pc, live, &parameters)?;
                    let resume = cg.b.create_block();
                    switch.set_entry(u128::from(pc), resume);
                    cg.b.switch_to_block(resume);
                    cg.reload_continuation(live, &parameters)?;
                } else if let I::Return(value) = instruction {
                    let value = match value {
                        Some(v) => cg.value(v)?,
                        None => cg.k(0),
                    };
                    cg.runtime("rcc_native_process_return", &[value])?;
                    let zero = cg.k(0);
                    cg.b.ins().return_(&[zero]);
                } else {
                    cg.instruction(instruction, block.id)
                        .map_err(|e| format!("{} block {}: {e}", function.name, block.id))?;
                }
            }
        }
        cg.emit_edges()?;
        cg.b.switch_to_block(dispatch);
        let pc = cg.runtime("rcc_native_process_pc", &[])?;
        let invalid = cg.b.create_block();
        switch.emit(&mut cg.b, pc, invalid);
        cg.b.switch_to_block(invalid);
        cg.runtime("rcc_native_process_bad_pc", &[])?;
        let zero: ClValue = cg.k(0);
        cg.b.ins().return_(&[zero]);
        cg.b.seal_all_blocks();
        cg.b.finalize(cg.compiler.object.target_config());
    }
    compiler
        .object
        .define_function(compiler.functions[&function.name], &mut context)
        .map_err(|e| {
            format!(
                "resumable native compilation {}: {e:?}\n{}",
                function.name,
                context.func.display()
            )
        })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sparse_ssa_ids_use_dense_slots_without_spilling_parameters() {
        let live = BTreeSet::from([3, 1000, 1000000, u32::MAX]);
        let parameters = BTreeMap::from([(1000, 0)]);
        assert_eq!(
            continuation_slots(&live, &parameters),
            BTreeMap::from([(3, 0), (1000000, 1), (u32::MAX, 2)])
        );
        assert!(continuation_slots(&BTreeSet::from([1000]), &parameters).is_empty());
    }
}
