//! Values needed after a suspension. Phi operands belong to their incoming
//! edges, rather than all predecessors, including loop back edges.
use rcc_ir::ir::{Function, Instruction as I, Value};
use std::collections::{BTreeMap, BTreeSet};

type Set = BTreeSet<u32>;
pub(super) struct Liveness {
    pub after: Vec<Vec<Set>>,
}

fn value_uses(value: &Value, uses: &mut Set) {
    match value {
        Value::Temp(id) => {
            uses.insert(*id);
        }
        Value::FatPtr(p) => value_uses(&p.addr, uses),
        Value::AddressOffset { base, .. } => value_uses(base, uses),
        Value::ConstantAggregate(values) => {
            for value in values {
                value_uses(value, uses);
            }
        }
        Value::Constant(_)
        | Value::Global(_)
        | Value::Function(_)
        | Value::ConstantArray(_)
        | Value::Undef => {}
    }
}

fn uses(instruction: &I) -> Set {
    let mut out = Set::new();
    match instruction {
        I::Binary { lhs, rhs, .. } => {
            value_uses(lhs, &mut out);
            value_uses(rhs, &mut out);
        }
        I::Unary { operand, .. } => value_uses(operand, &mut out),
        I::Load { ptr, .. } => value_uses(ptr, &mut out),
        I::Store { value, ptr, .. } => {
            value_uses(value, &mut out);
            value_uses(ptr, &mut out);
        }
        I::GetElementPtr { ptr, indices, .. } => {
            value_uses(ptr, &mut out);
            for index in indices {
                value_uses(index, &mut out);
            }
        }
        I::Alloca { count, .. } | I::Return(count) => {
            if let Some(value) = count {
                value_uses(value, &mut out);
            }
        }
        I::Call { function, args, .. } => {
            value_uses(function, &mut out);
            for value in args {
                value_uses(value, &mut out);
            }
        }
        I::Intrinsic { args, .. } => {
            for value in args {
                value_uses(value, &mut out);
            }
        }
        I::BranchCond { condition, .. } => value_uses(condition, &mut out),
        I::Cast { value, .. } => value_uses(value, &mut out),
        I::Select {
            condition,
            true_value,
            false_value,
            ..
        } => {
            for value in [condition, true_value, false_value] {
                value_uses(value, &mut out);
            }
        }
        I::InlineAsm {
            inputs, outputs, ..
        } => {
            for operand in inputs.iter().chain(outputs) {
                value_uses(&operand.value, &mut out);
            }
        }
        I::Phi { .. } | I::Branch(_) | I::DebugLoc { .. } | I::Comment(_) => {}
    }
    out
}

impl Liveness {
    pub fn before(&self, block: usize, position: usize, instruction: &I) -> Set {
        let mut live = self.after[block][position].clone();
        if let Some((id, _)) = super::emit::result_type(instruction) {
            live.remove(&id);
        }
        live.extend(uses(instruction));
        live
    }

    pub fn analyze(function: &Function) -> Result<Self, String> {
        let mut input = BTreeMap::<u32, Set>::new();
        let mut output = input.clone();
        let blocks: BTreeMap<_, _> = function.blocks.iter().map(|b| (b.id, b)).collect();
        for block in &function.blocks {
            input.insert(block.id, Set::new());
            output.insert(block.id, Set::new());
        }
        loop {
            let mut changed = false;
            for block in function.blocks.iter().rev() {
                let successors = match block
                    .instructions
                    .iter()
                    .rev()
                    .find(|i| matches!(i, I::Branch(_) | I::BranchCond { .. } | I::Return(_)))
                {
                    Some(I::Branch(to)) => vec![*to],
                    Some(I::BranchCond {
                        true_label,
                        false_label,
                        ..
                    }) => vec![*true_label, *false_label],
                    _ => vec![],
                };
                let mut live = Set::new();
                for to in successors {
                    let next = blocks
                        .get(&to)
                        .ok_or_else(|| format!("unknown native block {to}"))?;
                    live.extend(&input[&to]);
                    for instruction in &next.instructions {
                        if let I::Phi { incoming, .. } = instruction {
                            for (value, from) in incoming {
                                if *from == block.id {
                                    value_uses(value, &mut live);
                                }
                            }
                        }
                    }
                }
                changed |= output[&block.id] != live;
                output.insert(block.id, live.clone());
                for instruction in block.instructions.iter().rev() {
                    if let Some((id, _)) = super::emit::result_type(instruction) {
                        live.remove(&id);
                    }
                    live.extend(uses(instruction));
                }
                changed |= input[&block.id] != live;
                input.insert(block.id, live);
            }
            if !changed {
                break;
            }
        }
        let after = function
            .blocks
            .iter()
            .map(|block| {
                let mut live = output[&block.id].clone();
                let mut after = vec![Set::new(); block.instructions.len()];
                for (position, instruction) in block.instructions.iter().enumerate().rev() {
                    after[position] = live.clone();
                    if let Some((id, _)) = super::emit::result_type(instruction) {
                        live.remove(&id);
                    }
                    live.extend(uses(instruction));
                }
                after
            })
            .collect();
        Ok(Self { after })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::{BasicBlock, IrBinaryOp, IrType};
    fn block(id: u32, instructions: Vec<I>) -> BasicBlock {
        let mut block = BasicBlock::new(id);
        block.instructions = instructions;
        block
    }
    fn call(result: u32) -> I {
        I::Call {
            result: Some(result),
            function: Value::Function("suspend".into()),
            args: vec![],
            result_type: IrType::I16,
            stack_args_from: None,
        }
    }
    #[test]
    fn call_only_operands_die_while_nested_pointer_bases_survive() {
        let mut f = Function::new("pointer".into(), IrType::I16);
        let mut suspend = call(8);
        if let I::Call { args, .. } = &mut suspend {
            args.push(Value::Temp(3));
        }
        let pointer = Value::FatPtr(rcc_ir::ir::FatPointer {
            addr: Box::new(Value::AddressOffset {
                base: Box::new(Value::Temp(5)),
                cells: 1,
            }),
            bank: rcc_ir::BankTag::Mixed,
        });
        f.blocks = vec![block(
            0,
            vec![
                suspend,
                I::Store {
                    ptr: pointer,
                    value: Value::Temp(8),
                    value_type: IrType::I16,
                    volatile: false,
                },
                I::Return(None),
            ],
        )];
        assert_eq!(
            Liveness::analyze(&f).unwrap().after[0][0],
            Set::from([5, 8])
        );
    }
    #[test]
    fn phi_inputs_are_live_only_on_the_corresponding_edge() {
        let mut f = Function::new("diamond".into(), IrType::I16);
        f.blocks = vec![
            block(0, vec![call(8), I::Branch(2)]),
            block(1, vec![call(9), I::Branch(2)]),
            block(
                2,
                vec![
                    I::Phi {
                        result: 3,
                        incoming: vec![(Value::Temp(1), 0), (Value::Temp(2), 1)],
                        result_type: IrType::I16,
                    },
                    I::Return(Some(Value::Temp(3))),
                ],
            ),
        ];
        let live = Liveness::analyze(&f).unwrap();
        assert_eq!(live.after[0][0], Set::from([1]));
        assert_eq!(live.after[1][0], Set::from([2]));
    }
    #[test]
    fn loop_back_edges_keep_phi_inputs_and_drop_dead_values() {
        let mut f = Function::new("loop".into(), IrType::I16);
        f.blocks = vec![
            block(0, vec![I::Branch(1)]),
            block(
                1,
                vec![
                    I::Phi {
                        result: 2,
                        incoming: vec![(Value::Constant(0), 0), (Value::Temp(4), 1)],
                        result_type: IrType::I16,
                    },
                    I::Binary {
                        result: 4,
                        lhs: Value::Temp(2),
                        rhs: Value::Constant(1),
                        op: IrBinaryOp::Add,
                        result_type: IrType::I16,
                    },
                    call(5),
                    I::BranchCond {
                        condition: Value::Temp(5),
                        true_label: 1,
                        false_label: 2,
                    },
                ],
            ),
            block(2, vec![I::Return(Some(Value::Temp(4)))]),
        ];
        assert_eq!(
            Liveness::analyze(&f).unwrap().after[1][2],
            Set::from([4, 5])
        );
    }
}
