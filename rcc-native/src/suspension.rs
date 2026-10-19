//! Conservative direct-call islands inside resumable programs. A bounded,
//! acyclic call/CFG tree cannot reach a suspension or require a loop checkpoint.
//! Public function tokens still name their resumable entry points.
use rcc_ir::ir::{Function, Instruction as I, IrType, Module, Value};
use std::collections::{BTreeMap, BTreeSet};

const MAX_COST: usize = 256;

fn acyclic(function: &Function) -> bool {
    fn visit(
        id: u32,
        edges: &BTreeMap<u32, Vec<u32>>,
        active: &mut BTreeSet<u32>,
        done: &mut BTreeSet<u32>,
    ) -> bool {
        if done.contains(&id) {
            return true;
        }
        if !active.insert(id) {
            return false;
        }
        let Some(next) = edges.get(&id) else {
            return false;
        };
        if next.iter().any(|to| !visit(*to, edges, active, done)) {
            return false;
        }
        active.remove(&id);
        done.insert(id);
        true
    }
    if function.blocks.len() > MAX_COST {
        return false;
    }
    let edges = function
        .blocks
        .iter()
        .map(|block| {
            let successors = match block.instructions.last() {
                Some(I::Branch(to)) => vec![*to],
                Some(I::BranchCond {
                    true_label,
                    false_label,
                    ..
                }) => vec![*true_label, *false_label],
                Some(I::Return(_)) => vec![],
                _ => vec![u32::MAX], // Malformed CFGs never qualify.
            };
            (block.id, successors)
        })
        .collect();
    let mut done = BTreeSet::new();
    function
        .blocks
        .iter()
        .all(|block| visit(block.id, &edges, &mut BTreeSet::new(), &mut done))
}

pub(super) fn direct_functions(module: &Module) -> BTreeSet<String> {
    let mut costs = BTreeMap::<String, usize>::new();
    loop {
        let old = costs.len();
        for function in &module.functions {
            // Pointer returns can carry aggregate results in released logical
            // stack storage. Keep their existing return/checkpoint protocol.
            if function.is_external
                || costs.contains_key(&function.name)
                || matches!(
                    function.return_type,
                    IrType::FatPtr(_) | IrType::Struct { .. } | IrType::Array { .. }
                )
                || !acyclic(function)
            {
                continue;
            }
            let mut cost = 0usize;
            let safe = function
                .blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .all(|inst| {
                    cost = cost.saturating_add(1);
                    if let I::Call {
                        function: Value::Global(name) | Value::Function(name),
                        ..
                    } = inst
                    {
                        let Some(callee) = costs.get(name) else {
                            return false;
                        };
                        cost = cost.saturating_add(*callee);
                    } else if matches!(inst, I::Call { .. } | I::InlineAsm { .. }) {
                        return false;
                    }
                    cost <= MAX_COST
                });
            if safe {
                costs.insert(function.name.clone(), cost);
            }
        }
        if costs.len() == old {
            return costs.into_keys().collect();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::BasicBlock;
    fn function(name: &str, instructions: Vec<I>) -> Function {
        let mut f = Function::new(name.into(), IrType::I16);
        let mut block = BasicBlock::new(0);
        block.instructions = instructions;
        f.blocks.push(block);
        f
    }
    fn call(name: &str) -> I {
        I::Call {
            result: None,
            function: Value::Function(name.into()),
            args: vec![],
            result_type: IrType::Void,
            stack_args_from: None,
        }
    }
    #[test]
    fn closure_excludes_cycles_unknown_calls_services_and_indirect_calls() {
        let mut module = Module::new("analysis".into());
        for f in [
            function("parent", vec![call("leaf"), I::Return(None)]),
            function("leaf", vec![I::Return(None)]),
            function("recursive", vec![call("recursive"), I::Return(None)]),
            function("service", vec![call("rcc_process_yield"), I::Return(None)]),
            function("unknown", vec![call("unknown_external"), I::Return(None)]),
            function("loop", vec![I::Branch(0)]),
        ] {
            module.add_function(f);
        }
        let mut indirect = function("indirect", vec![call("leaf"), I::Return(None)]);
        if let I::Call { function, .. } = &mut indirect.blocks[0].instructions[0] {
            *function = Value::Temp(0);
        }
        module.add_function(indirect);
        assert_eq!(
            direct_functions(&module),
            BTreeSet::from(["leaf".into(), "parent".into()])
        );
    }
    #[test]
    fn total_call_tree_cost_and_pointer_returns_are_conservative() {
        let mut module = Module::new("bounds".into());
        let mut leaf = function("leaf", vec![I::Comment("padding".into()); MAX_COST - 1]);
        leaf.blocks[0].instructions.push(I::Return(None));
        module.add_function(leaf);
        module.add_function(function("big", vec![call("leaf"), I::Return(None)]));
        let mut aggregate = function("aggregate", vec![I::Return(None)]);
        aggregate.return_type = IrType::FatPtr(Box::new(IrType::I16));
        module.add_function(aggregate);
        assert_eq!(direct_functions(&module), BTreeSet::from(["leaf".into()]));
    }
}
