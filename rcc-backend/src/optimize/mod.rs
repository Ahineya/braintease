//! Conservative, target-width-aware IR optimizations. Memory operations retain
//! their order; a pure expression is never a license to speculate a load.

mod constants;
mod inline;
mod locals;
mod loops;
mod narrow;
mod pointers;
pub(crate) mod cfg;
mod ssa;
mod phi;
mod reachability;
mod redundancy;
#[cfg(test)]
mod tests;

use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction, Value};
use std::collections::{HashMap, HashSet};

pub use constants::fold_binary;
pub use pointers::preserve_pointer_banks;
pub use phi::destroy_phis;
pub use reachability::{function_symbols, function_has_opaque_asm, value_symbols};
pub(crate) use inline::safe_asm;

pub fn optimize_module(module: &mut rcc_frontend::ir::Module) {
    for function in &mut module.functions { optimize_function(function); }
    inline::inline_small_functions(module);
    for function in &mut module.functions {
        ssa::promote_integer_locals(function);
        redundancy::eliminate_redundancy(function);
        optimize_function(function);
    }
}

pub fn optimize_function(function: &mut Function) {
    if function.is_external {
        return;
    }
    locals::promote_single_store_locals(function);
    constants::simplify(function);
    narrow::narrow_gep_indices(function);
    eliminate_dead_expressions(function);
    loops::hoist_invariants(function);
}

pub(crate) fn result(inst: &Instruction) -> Option<TempId> {
    match inst {
        Instruction::Binary { result, .. }
        | Instruction::Unary { result, .. }
        | Instruction::Load { result, .. }
        | Instruction::GetElementPtr { result, .. }
        | Instruction::Alloca { result, .. }
        | Instruction::Phi { result, .. }
        | Instruction::Cast { result, .. }
        | Instruction::Select { result, .. } => Some(*result),
        Instruction::Call { result, .. } | Instruction::Intrinsic { result, .. } => *result,
        _ => None,
    }
}

pub(crate) fn value_temps(value: &Value, temps: &mut HashSet<TempId>) {
    match value {
        Value::Temp(t) => {
            temps.insert(*t);
        }
        Value::FatPtr(p) => value_temps(&p.addr, temps),
        Value::AddressOffset { base, .. } => value_temps(base, temps),
        Value::ConstantAggregate(values) => {
            for v in values {
                value_temps(v, temps);
            }
        }
        _ => {}
    }
}

fn operands(inst: &Instruction) -> Vec<&Value> {
    match inst {
        Instruction::Binary { lhs, rhs, .. } => vec![lhs, rhs],
        Instruction::Unary { operand, .. } => vec![operand],
        Instruction::Load { ptr, .. } => vec![ptr],
        Instruction::Store { value, ptr, .. } => vec![value, ptr],
        Instruction::GetElementPtr { ptr, indices, .. } => {
            std::iter::once(ptr).chain(indices).collect()
        }
        Instruction::Alloca { count, .. } | Instruction::Return(count) => count.iter().collect(),
        Instruction::Call { function, args, .. } => std::iter::once(function).chain(args).collect(),
        Instruction::Intrinsic { args, .. } => args.iter().collect(),
        Instruction::BranchCond { condition, .. } => vec![condition],
        Instruction::Phi { incoming, .. } => incoming.iter().map(|(v, _)| v).collect(),
        Instruction::Cast { value, .. } => vec![value],
        Instruction::Select {
            condition,
            true_value,
            false_value,
            ..
        } => vec![condition, true_value, false_value],
        // Output operands may carry addresses and read/write constraints.
        Instruction::InlineAsm {
            inputs, outputs, ..
        } => inputs.iter().chain(outputs).map(|o| &o.value).collect(),
        Instruction::Branch(_) | Instruction::DebugLoc { .. } | Instruction::Comment(_) => vec![],
    }
}

pub(crate) fn used_temps(inst: &Instruction) -> HashSet<TempId> {
    let mut temps = HashSet::new();
    for value in operands(inst) {
        value_temps(value, &mut temps);
    }
    temps
}

fn pure(inst: &Instruction) -> bool {
    matches!(
        inst,
        Instruction::Binary { .. } | Instruction::Unary { .. } | Instruction::GetElementPtr { .. }
    )
}

fn eliminate_dead_expressions(function: &mut Function) {
    // Work backwards through a use graph instead of repeatedly scanning long
    // generated initializer functions to discover one dead expression at a time.
    let mut uses = HashMap::<TempId, usize>::new();
    let mut definitions = HashMap::new();
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            for t in used_temps(inst) {
                *uses.entry(t).or_default() += 1;
            }
            if let Some(t) = result(inst) {
                definitions.insert(t, (bi, ii));
            }
        }
    }
    let mut work: Vec<_> = definitions
        .keys()
        .filter(|t| !uses.contains_key(t))
        .copied()
        .collect();
    let mut removed = HashSet::new();
    while let Some(t) = work.pop() {
        let &(bi, ii) = &definitions[&t];
        let inst = &function.blocks[bi].instructions[ii];
        if !pure(inst) || !removed.insert((bi, ii)) {
            continue;
        }
        for operand in used_temps(inst) {
            let count = uses.get_mut(&operand).unwrap();
            *count -= 1;
            if *count == 0 && definitions.contains_key(&operand) {
                work.push(operand);
            }
        }
    }
    for (bi, block) in function.blocks.iter_mut().enumerate() {
        let mut ii = 0;
        block.instructions.retain(|_| {
            let keep = !removed.contains(&(bi, ii));
            ii += 1;
            keep
        });
    }
}
