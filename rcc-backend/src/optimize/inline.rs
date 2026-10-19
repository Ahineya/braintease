//! Bounded inlining of small, acyclic leaf functions. Operands are evaluated
//! by the caller before the call; cloned parameters retain their declared type.
//! Opaque/frame-dependent assembly, recursion and varargs remain real calls.

use super::{operands, result, value_temps};
use rcc_ir::ir::{BasicBlock, Function, Instruction as I, IrBinaryOp, IrType, Module, Value};
use std::collections::{HashMap, HashSet};

const MAX_BODY: usize = 48;
const MAX_GROWTH: usize = 192;

pub(super) fn map_value(value: &mut Value, map: &HashMap<u32, u32>) {
    match value {
        Value::Temp(t) => *t = *map.get(t).unwrap_or(t),
        Value::FatPtr(p) => map_value(&mut p.addr, map),
        Value::AddressOffset { base, .. } => map_value(base, map),
        Value::ConstantAggregate(v) => v.iter_mut().for_each(|v| map_value(v, map)),
        _ => {}
    }
}

fn rename(inst: &mut I, temps: &HashMap<u32, u32>, labels: &HashMap<u32, u32>) {
    match inst {
        I::Binary {
            result, lhs, rhs, ..
        } => {
            *result = temps[result];
            map_value(lhs, temps);
            map_value(rhs, temps);
        }
        I::Unary {
            result, operand, ..
        } => {
            *result = temps[result];
            map_value(operand, temps);
        }
        I::Load { result, ptr, .. } => {
            *result = temps[result];
            map_value(ptr, temps);
        }
        I::Store { value, ptr, .. } => {
            map_value(value, temps);
            map_value(ptr, temps);
        }
        I::GetElementPtr {
            result,
            ptr,
            indices,
            ..
        } => {
            *result = temps[result];
            map_value(ptr, temps);
            indices.iter_mut().for_each(|v| map_value(v, temps));
        }
        I::Alloca { result, count, .. } => {
            *result = temps[result];
            if let Some(v) = count {
                map_value(v, temps);
            }
        }
        I::Cast { result, value, .. } => {
            *result = temps[result];
            map_value(value, temps);
        }
        I::Select {
            result,
            condition,
            true_value,
            false_value,
            ..
        } => {
            *result = temps[result];
            map_value(condition, temps);
            map_value(true_value, temps);
            map_value(false_value, temps);
        }
        I::Return(v) => {
            if let Some(v) = v {
                map_value(v, temps);
            }
        }
        I::Branch(l) => *l = labels[l],
        I::BranchCond {
            condition,
            true_label,
            false_label,
        } => {
            map_value(condition, temps);
            *true_label = labels[true_label];
            *false_label = labels[false_label];
        }
        I::InlineAsm {
            outputs, inputs, ..
        } => {
            for operand in outputs.iter_mut().chain(inputs) {
                map_value(&mut operand.value, temps);
            }
        }
        I::DebugLoc { .. } | I::Comment(_) => {}
        I::Call { .. } | I::Intrinsic { .. } | I::Phi { .. } => {
            unreachable!("not an inline candidate")
        }
    }
}

fn scalar(ty: &IrType) -> bool {
    ty.is_integer() || ty.is_pointer()
}

pub(crate) fn safe_asm(assembly: &str, clobbers: &[String]) -> bool {
    // Only operand-driven single instructions. No labels, branches, implicit
    // frame registers or hard-coded scratch registers that could hit the caller.
    if clobbers.iter().any(|c| c != "memory") {
        return false;
    }
    let words: Vec<_> = assembly
        .split(|c: char| c.is_whitespace() || c == ',')
        .filter(|s| !s.is_empty())
        .collect();
    if words.len() != 4
        || !matches!(
            words[0].to_ascii_uppercase().as_str(),
            "LOAD" | "STORE" | "ADD" | "SUB" | "AND" | "OR" | "XOR" | "SLT" | "SLTU"
        )
    {
        return false;
    }
    words[1..].iter().all(|s| {
        s.eq_ignore_ascii_case("R0")
            || s.strip_prefix('%')
                .is_some_and(|n| !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()))
    })
}

fn candidate(f: &Function) -> bool {
    if f.is_external
        || f.is_vararg
        || f.blocks.is_empty()
        || !(f.return_type == IrType::Void || scalar(&f.return_type))
        || f.parameters.iter().any(|(_, t)| !scalar(t))
    {
        return false;
    }
    if f.blocks.iter().map(|b| b.instructions.len()).sum::<usize>() > MAX_BODY {
        return false;
    }
    let Ok(cfg) = super::cfg::Cfg::new(f) else {
        return false;
    };
    // Block layout does not imply execution order: an acyclic early-return
    // helper can branch backward to a join placed earlier in the block vector.
    fn acyclic(index: usize, successors: &[Vec<usize>], colors: &mut [u8]) -> bool {
        if colors[index] != 0 {
            return colors[index] == 2;
        }
        colors[index] = 1;
        if successors[index]
            .iter()
            .any(|&next| !acyclic(next, successors, colors))
        {
            return false;
        }
        colors[index] = 2;
        true
    }
    let mut colors = vec![0; f.blocks.len()];
    if (0..f.blocks.len()).any(|i| !acyclic(i, &cfg.successors, &mut colors)) {
        return false;
    }
    for block in &f.blocks {
        if !block.has_terminator() {
            return false;
        }
        for inst in &block.instructions {
            match inst {
                I::Call { .. } | I::Intrinsic { .. } | I::Phi { .. } => return false,
                I::Alloca {
                    alloc_type, count, ..
                } if !scalar(alloc_type) || !matches!(count, None | Some(Value::Constant(1))) => {
                    return false
                }
                I::InlineAsm {
                    assembly,
                    outputs,
                    clobbers,
                    ..
                } if !safe_asm(assembly, clobbers)
                    || outputs.iter().any(|o| !matches!(o.value, Value::Temp(_))) =>
                {
                    return false
                }
                _ => {}
            }
        }
    }
    true
}

fn typed_copy(result: u32, value: Value, ty: &IrType, next_temp: &mut u32) -> Vec<I> {
    if ty.is_pointer() {
        // A pointer's bank is part of its value. The generic Cast lowerer is
        // scalar-only and cannot copy an explicit FatPtr argument or attach
        // bank metadata to its destination. A private typed home preserves
        // both words for Temp, tagged FatPtr and relocated global arguments.
        let slot = *next_temp;
        *next_temp += 1;
        vec![
            I::Alloca {
                result: slot,
                alloc_type: ty.clone(),
                count: None,
                result_type: IrType::FatPtr(Box::new(ty.clone())),
            },
            I::Store {
                value,
                ptr: Value::Temp(slot),
                value_type: ty.clone(),
                volatile: false,
            },
            I::Load {
                result,
                ptr: Value::Temp(slot),
                result_type: ty.clone(),
                volatile: false,
            },
        ]
    } else {
        vec![I::Binary {
            result,
            op: IrBinaryOp::Add,
            lhs: value,
            rhs: Value::Constant(0),
            result_type: ty.clone(),
        }]
    }
}

pub(super) fn merge_linear_blocks(f: &mut Function) {
    // The frontend can retain unreachable instructions after a terminator.
    // Such blocks cannot be merged using only their final instruction's edges.
    if super::cfg::Cfg::new(f).is_err() {
        return;
    }
    loop {
        let mut counts = HashMap::<u32, usize>::new();
        for b in &f.blocks {
            match b.instructions.last() {
                Some(I::Branch(l)) => *counts.entry(*l).or_default() += 1,
                Some(I::BranchCond {
                    true_label,
                    false_label,
                    ..
                }) => {
                    *counts.entry(*true_label).or_default() += 1;
                    *counts.entry(*false_label).or_default() += 1;
                }
                Some(I::Return(_)) => {}
                _ => return, // no complete CFG; do not assume no fallthrough
            }
        }
        let pair = f.blocks.iter().enumerate().find_map(|(i, b)| {
            if let Some(I::Branch(l)) = b.instructions.last() {
                if *l != b.id && counts.get(l) == Some(&1) {
                    if let Some(j) = f.blocks.iter().position(|b| b.id == *l) {
                        if j != 0
                            && !f.blocks[j]
                                .instructions
                                .iter()
                                .any(|i| matches!(i, I::Phi { .. }))
                        {
                            return Some((i, j));
                        }
                    }
                }
            }
            None
        });
        let Some((i, j)) = pair else {
            break;
        };
        let id = f.blocks[i].id;
        let removed = f.blocks.remove(j);
        let i = if j < i { i - 1 } else { i };
        f.blocks[i].instructions.pop();
        f.blocks[i].instructions.extend(removed.instructions);
        f.blocks[i].is_loop_condition |= removed.is_loop_condition;
        for b in &mut f.blocks {
            for inst in &mut b.instructions {
                if let I::Phi { incoming, .. } = inst {
                    for (_, pred) in incoming {
                        if *pred == removed.id {
                            *pred = id;
                        }
                    }
                }
            }
        }
    }
}

pub(super) fn inline_small_functions(module: &mut Module) {
    let initial: Vec<_> = module
        .functions
        .iter()
        .map(|f| f.blocks.iter().map(|b| b.instructions.len()).sum::<usize>())
        .collect();
    // Bottom-up rounds expose wrappers after their leaf calls have disappeared.
    // No recursive function can become a leaf, and growth is capped per caller.
    for _ in 0..4 {
        let candidates: HashMap<_, _> = module
            .functions
            .iter()
            .filter(|f| candidate(f))
            .map(|f| (f.name.clone(), f.clone()))
            .collect();
        let mut changed = false;
        for (fi, caller) in module.functions.iter_mut().enumerate() {
            if caller.is_external {
                continue;
            }
            let mut temps: HashSet<u32> = caller.parameters.iter().map(|(t, _)| *t).collect();
            for inst in caller.blocks.iter().flat_map(|b| &b.instructions) {
                if let Some(t) = result(inst) {
                    temps.insert(t);
                }
                for operand in operands(inst) {
                    value_temps(operand, &mut temps);
                }
            }
            let mut next_temp = temps.into_iter().max().unwrap_or(0) + 1;
            let mut next_label = caller.blocks.iter().map(|b| b.id).max().unwrap_or(0) + 1;
            let mut size = caller
                .blocks
                .iter()
                .map(|b| b.instructions.len())
                .sum::<usize>();
            let mut bi = 0;
            while bi < caller.blocks.len() {
                let site =
                    caller.blocks[bi]
                        .instructions
                        .iter()
                        .enumerate()
                        .find_map(|(ii, inst)| {
                            if let I::Call {
                                function: Value::Function(name) | Value::Global(name),
                                args,
                                stack_args_from: None,
                                ..
                            } = inst
                            {
                                if let Some(callee) = candidates.get(name) {
                                    let parameter_copies: usize = callee
                                        .parameters
                                        .iter()
                                        .map(|(_, ty)| if ty.is_pointer() { 3 } else { 1 })
                                        .sum();
                                    let growth = callee
                                        .blocks
                                        .iter()
                                        .map(|b| b.instructions.len())
                                        .sum::<usize>()
                                        + parameter_copies
                                        + 4;
                                    if name != &caller.name
                                        && args.len() == callee.parameters.len()
                                        && size + growth <= initial[fi] + MAX_GROWTH
                                    {
                                        return Some((ii, callee.clone(), growth));
                                    }
                                }
                            }
                            None
                        });
                let Some((ii, callee, growth)) = site else {
                    bi += 1;
                    continue;
                };
                let I::Call {
                    result: output,
                    args,
                    result_type,
                    ..
                } = caller.blocks[bi].instructions[ii].clone()
                else {
                    unreachable!()
                };
                let mut mapping = HashMap::new();
                for (t, _) in &callee.parameters {
                    mapping.insert(*t, next_temp);
                    next_temp += 1;
                }
                for inst in callee.blocks.iter().flat_map(|b| &b.instructions) {
                    if let Some(t) = result(inst) {
                        mapping.entry(t).or_insert_with(|| {
                            let t = next_temp;
                            next_temp += 1;
                            t
                        });
                    }
                    // Asm can define several SSA values, so result(inst) cannot
                    // represent its outputs. Every cloned output needs the same
                    // fresh identity treatment as an ordinary instruction result.
                    if let I::InlineAsm { outputs, .. } = inst {
                        for output in outputs {
                            if let Value::Temp(t) = output.value {
                                mapping.entry(t).or_insert_with(|| {
                                    let t = next_temp;
                                    next_temp += 1;
                                    t
                                });
                            }
                        }
                    }
                }
                let labels: HashMap<_, _> = callee
                    .blocks
                    .iter()
                    .map(|b| {
                        let l = next_label;
                        next_label += 1;
                        (b.id, l)
                    })
                    .collect();
                let continuation = next_label;
                next_label += 1;
                let original_id = caller.blocks[bi].id;
                let tail = caller.blocks[bi].instructions.split_off(ii + 1);
                caller.blocks[bi].instructions.pop();
                let result_slot = output.map(|_| {
                    let t = next_temp;
                    next_temp += 1;
                    t
                });
                if let Some(slot) = result_slot {
                    caller.blocks[bi].instructions.push(I::Alloca {
                        result: slot,
                        alloc_type: result_type.clone(),
                        count: None,
                        result_type: IrType::FatPtr(Box::new(result_type.clone())),
                    });
                }
                for ((param, ty), arg) in callee.parameters.iter().zip(args) {
                    caller.blocks[bi].instructions.extend(typed_copy(
                        mapping[param],
                        arg,
                        ty,
                        &mut next_temp,
                    ));
                }
                caller.blocks[bi]
                    .instructions
                    .push(I::Branch(labels[&callee.blocks[0].id]));
                let mut cloned = Vec::new();
                for old in &callee.blocks {
                    let mut block = BasicBlock::new(labels[&old.id]);
                    for mut inst in old.instructions.clone() {
                        rename(&mut inst, &mapping, &labels);
                        match inst {
                            I::Return(value) => {
                                if let (Some(slot), Some(value)) = (result_slot, value) {
                                    block.instructions.push(I::Store {
                                        value,
                                        ptr: Value::Temp(slot),
                                        value_type: result_type.clone(),
                                        volatile: false,
                                    });
                                }
                                block.instructions.push(I::Branch(continuation));
                            }
                            other => block.instructions.push(other),
                        }
                    }
                    cloned.push(block);
                }
                let mut after = BasicBlock::new(continuation);
                if let (Some(result), Some(slot)) = (output, result_slot) {
                    after.instructions.push(I::Load {
                        result,
                        ptr: Value::Temp(slot),
                        result_type,
                        volatile: false,
                    });
                }
                after.instructions.extend(tail);
                // Existing Phi edges out of the split block now leave its tail.
                for block in &mut caller.blocks {
                    for inst in &mut block.instructions {
                        if let I::Phi { incoming, .. } = inst {
                            for (_, pred) in incoming {
                                if *pred == original_id {
                                    *pred = continuation;
                                }
                            }
                        }
                    }
                }
                cloned.push(after);
                caller.blocks.splice(bi + 1..bi + 1, cloned);
                size += growth;
                changed = true;
            }
            merge_linear_blocks(caller);
            // Frame slots have function lifetime. A backward edge can merge
            // an inline entry into a block laid out after its uses. The
            // lowerer visits blocks in layout order and must register these
            // fixed homes before any load/store resolves their pointer banks.
            let mut homes = Vec::new();
            for block in &mut caller.blocks {
                block.instructions.retain(|inst| {
                    if matches!(inst, I::Alloca { count: None | Some(Value::Constant(_)), .. }) {
                        homes.push(inst.clone());
                        false
                    } else {
                        true
                    }
                });
            }
            if let Some(entry) = caller.blocks.first_mut() {
                entry.instructions.splice(0..0, homes);
            }
            super::optimize_function(caller);
            // These caches are advisory; rebuild them from terminators.
            let edges: Vec<_> = caller
                .blocks
                .iter()
                .flat_map(|b| match b.instructions.last() {
                    Some(I::Branch(l)) => vec![(b.id, *l)],
                    Some(I::BranchCond {
                        true_label,
                        false_label,
                        ..
                    }) => vec![(b.id, *true_label), (b.id, *false_label)],
                    _ => vec![],
                })
                .collect();
            for b in &mut caller.blocks {
                b.predecessors = edges
                    .iter()
                    .filter_map(|(p, s)| (*s == b.id).then_some(*p))
                    .collect();
                b.successors = edges
                    .iter()
                    .filter_map(|(p, s)| (*p == b.id).then_some(*s))
                    .collect();
            }
        }
        if !changed {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::AsmOperandIR;
    fn ret(name: &str) -> Function {
        let mut f = Function::new(name.into(), IrType::I32);
        f.parameters.push((0, IrType::I32));
        let mut b = BasicBlock::new(0);
        b.instructions.push(I::Return(Some(Value::Temp(0))));
        f.blocks.push(b);
        f
    }
    #[test]
    fn renumbers_inlined_temporaries_without_collisions() {
        let mut m = Module::new("inline".into());
        let helper = ret("helper");
        let mut caller = ret("caller");
        caller.blocks[0].instructions.insert(
            0,
            I::Call {
                result: Some(1),
                function: Value::Function("helper".into()),
                args: vec![Value::Temp(0)],
                result_type: IrType::I32,
                stack_args_from: None,
            },
        );
        caller.blocks[0].instructions[1] = I::Return(Some(Value::Temp(1)));
        m.functions = vec![helper, caller];
        inline_small_functions(&mut m);
        let instructions: Vec<_> = m.functions[1]
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .collect();
        assert!(!instructions.iter().any(|i| matches!(i, I::Call { .. })));
        let defs: Vec<_> = instructions.iter().filter_map(|i| result(i)).collect();
        assert_eq!(defs.iter().collect::<HashSet<_>>().len(), defs.len());
    }
    #[test]
    fn recursion_varargs_and_opaque_assembly_are_not_candidates() {
        let mut f = ret("f");
        f.is_vararg = true;
        assert!(!candidate(&f));
        f.is_vararg = false;
        f.blocks[0].instructions.insert(
            0,
            I::InlineAsm {
                assembly: "LI S3, 17".into(),
                outputs: vec![],
                inputs: vec![],
                clobbers: vec!["s3".into()],
            },
        );
        assert!(!candidate(&f));
        assert!(safe_asm("STORE %0, R0, %1", &[]));
        assert!(!safe_asm("STORE %0, SB, SP", &[]));
        f.blocks[0].instructions[0] = I::Call {
            result: None,
            function: Value::Function("f".into()),
            args: vec![Value::Temp(0)],
            result_type: IrType::I32,
            stack_args_from: None,
        };
        assert!(!candidate(&f));
    }

    #[test]
    fn acyclic_backward_layout_edges_inline_but_cycles_do_not() {
        let mut helper = ret("helper");
        let mut later = BasicBlock::new(2);
        later.instructions.push(I::Branch(1));
        let mut join = BasicBlock::new(1);
        join.instructions.push(I::Return(Some(Value::Temp(0))));
        helper.blocks[0].instructions = vec![I::BranchCond {
            condition: Value::Temp(0),
            true_label: 2,
            false_label: 1,
        }];
        helper.blocks.extend([join, later]);
        assert!(candidate(&helper));
        let mut caller = ret("caller");
        caller.blocks[0].instructions = vec![
            I::Call {
                result: Some(1),
                function: Value::Global("helper".into()),
                args: vec![Value::Temp(0)],
                result_type: IrType::I32,
                stack_args_from: None,
            },
            I::Return(Some(Value::Temp(1))),
        ];
        let mut module = Module::new("backward_layout".into());
        module.functions = vec![helper.clone(), caller];
        inline_small_functions(&mut module);
        assert!(!module.functions[1]
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(i, I::Call { .. })));
        helper.blocks[1].instructions = vec![I::Branch(2)];
        assert!(!candidate(&helper));
    }

    #[test]
    fn inline_stack_homes_survive_backward_switch_layout() {
        let source = r#"module backward_switch {
          function i16 @digit(i16 %0) {
            block 0:
              %1 = alloca i16 -> i16*
              store i16 %0, fat(%1, mixed)
              %2 = load i16, fat(%1, mixed)
              %3 = sle i16 48, %2
              br %3, label 1, label 3
            block 1:
              %4 = load i16, fat(%1, mixed)
              %5 = sle i16 %4, 55
              br %5, label 2, label 3
            block 2:
              %6 = load i16, fat(%1, mixed)
              %7 = sub i16 %6, 48
              ret %7
            block 3:
              ret -1
          }
          function i16 @caller(i16 %0) {
            block 0:
              br label 3
            block 1:
              %1 = call i16 func @digit(%0)
              ret %1
            block 2:
              ret 42
            block 3:
              %2 = eq i16 %0, 120
              br %2, label 2, label 1
          }
        }"#;
        let mut module = rcc_ir::parse_module(source).unwrap();
        inline_small_functions(&mut module);
        let caller = &module.functions[1];
        assert!(!caller.blocks.iter().flat_map(|b| &b.instructions)
            .any(|i| matches!(i, I::Call { .. })));
        assert!(!caller.blocks.iter().skip(1).flat_map(|b| &b.instructions)
            .any(|i| matches!(i, I::Alloca { .. })));
        // This used to panic resolving the bank of the cloned parameter home.
        crate::lower::module::lower_module(&rcc_ir::parse_module(source).unwrap(), 64000, false).unwrap();
    }

    #[test]
    fn nonfinal_terminators_disable_candidate_and_block_merging() {
        for early in [
            I::Return(None),
            I::Branch(2),
            I::BranchCond {
                condition: Value::Temp(0),
                true_label: 1,
                false_label: 2,
            },
        ] {
            let mut f = ret("helper");
            f.blocks[0].instructions = vec![early, I::Branch(1)];
            for id in [1, 2] {
                let mut block = BasicBlock::new(id);
                block.instructions.push(I::Return(Some(Value::Temp(0))));
                f.blocks.push(block);
            }
            assert!(!candidate(&f));
            let before: Vec<_> = f
                .blocks
                .iter()
                .map(|b| (b.id, b.instructions.clone()))
                .collect();
            merge_linear_blocks(&mut f);
            let after: Vec<_> = f
                .blocks
                .iter()
                .map(|b| (b.id, b.instructions.clone()))
                .collect();
            assert_eq!(before, after);
        }
    }

    #[test]
    fn each_cloned_asm_output_has_a_fresh_definition() {
        let mut helper = Function::new("double_word".into(), IrType::I16);
        helper.parameters.push((0, IrType::I16));
        let mut block = BasicBlock::new(0);
        block.instructions = vec![
            I::InlineAsm {
                assembly: "ADD %0, %1, %1".into(),
                outputs: vec![AsmOperandIR {
                    constraint: "=r".into(),
                    value: Value::Temp(1),
                    tied_to: None,
                }],
                inputs: vec![AsmOperandIR {
                    constraint: "r".into(),
                    value: Value::Temp(0),
                    tied_to: None,
                }],
                clobbers: vec![],
            },
            I::Return(Some(Value::Temp(1))),
        ];
        helper.blocks.push(block);
        let mut caller = Function::new("caller".into(), IrType::I16);
        caller.parameters.push((1, IrType::I16));
        let mut block = BasicBlock::new(0);
        block.instructions = vec![
            I::Call {
                result: Some(2),
                function: Value::Global("double_word".into()),
                args: vec![Value::Temp(1)],
                result_type: IrType::I16,
                stack_args_from: None,
            },
            I::Call {
                result: Some(3),
                function: Value::Global("double_word".into()),
                args: vec![Value::Temp(2)],
                result_type: IrType::I16,
                stack_args_from: None,
            },
            I::Return(Some(Value::Temp(3))),
        ];
        caller.blocks.push(block);
        let mut module = Module::new("asm_outputs".into());
        module.functions = vec![helper, caller];
        inline_small_functions(&mut module);
        let instructions: Vec<_> = module.functions[1]
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .collect();
        assert!(!instructions.iter().any(|i| matches!(i, I::Call { .. })));
        let outputs: Vec<_> = instructions
            .iter()
            .filter_map(|i| match i {
                I::InlineAsm { outputs, .. } => Some(outputs[0].value.clone()),
                _ => None,
            })
            .collect();
        assert_eq!(outputs.len(), 2);
        assert_ne!(outputs[0], outputs[1]);
        assert!(outputs.iter().all(|v| v != &Value::Temp(1)));
    }
}
