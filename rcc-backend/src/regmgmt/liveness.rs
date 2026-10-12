//! Conservative IR liveness for reclaiming physical registers between instructions.
//!
//! This tracks every operand, including nested fat pointers and assembly outputs
//! (which are addresses/read-modify-write operands). Phi inputs are conservatively
//! live on every incoming edge. Spill locations and value metadata are not killed:
//! another block emitted later can still need them on an alternative runtime path.

use std::collections::{BTreeSet, HashMap};
use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction, Value};

#[derive(Default)]
pub(crate) struct FunctionLiveness {
    pub(crate) before: Vec<Vec<BTreeSet<TempId>>>,
    pub(crate) after: Vec<Vec<BTreeSet<TempId>>>,
}

fn value_uses(value: &Value, uses: &mut BTreeSet<TempId>) {
    match value {
        Value::Temp(id) => { uses.insert(*id); }
        Value::FatPtr(pointer) => value_uses(&pointer.addr, uses),
        Value::AddressOffset { base, .. } => value_uses(base, uses),
        Value::ConstantAggregate(values) => {
            for value in values { value_uses(value, uses); }
        }
        Value::Constant(_) | Value::Global(_) | Value::Function(_)
        | Value::ConstantArray(_) | Value::Undef => {}
    }
}

/// Exhaustive matching makes a new instruction or operand variant require an
/// explicit liveness decision rather than silently treating its operands as dead.
fn operands(instruction: &Instruction) -> (Option<TempId>, BTreeSet<TempId>) {
    let mut uses = BTreeSet::new();
    let definition = match instruction {
        Instruction::Binary { result, lhs, rhs, .. } => {
            value_uses(lhs, &mut uses);
            value_uses(rhs, &mut uses);
            Some(*result)
        }
        Instruction::Unary { result, operand, .. } => {
            value_uses(operand, &mut uses);
            Some(*result)
        }
        Instruction::Load { result, ptr, .. } => {
            value_uses(ptr, &mut uses);
            Some(*result)
        }
        Instruction::Store { value, ptr, .. } => {
            value_uses(value, &mut uses);
            value_uses(ptr, &mut uses);
            None
        }
        Instruction::GetElementPtr { result, ptr, indices, .. } => {
            value_uses(ptr, &mut uses);
            for index in indices { value_uses(index, &mut uses); }
            Some(*result)
        }
        Instruction::Alloca { result, count, .. } => {
            if let Some(count) = count { value_uses(count, &mut uses); }
            Some(*result)
        }
        Instruction::Call { result, function, args, .. } => {
            value_uses(function, &mut uses);
            for arg in args { value_uses(arg, &mut uses); }
            *result
        }
        Instruction::Intrinsic { result, args, .. } => {
            for arg in args { value_uses(arg, &mut uses); }
            *result
        }
        Instruction::Return(value) => {
            if let Some(value) = value { value_uses(value, &mut uses); }
            None
        }
        Instruction::BranchCond { condition, .. } => {
            value_uses(condition, &mut uses);
            None
        }
        Instruction::Phi { result, incoming, .. } => {
            for (value, _) in incoming { value_uses(value, &mut uses); }
            Some(*result)
        }
        Instruction::Cast { result, value, .. } => {
            value_uses(value, &mut uses);
            Some(*result)
        }
        Instruction::Select { result, condition, true_value, false_value, .. } => {
            value_uses(condition, &mut uses);
            value_uses(true_value, &mut uses);
            value_uses(false_value, &mut uses);
            Some(*result)
        }
        Instruction::InlineAsm { inputs, outputs, .. } => {
            for operand in inputs.iter().chain(outputs) {
                value_uses(&operand.value, &mut uses);
            }
            None
        }
        Instruction::Branch(_) | Instruction::DebugLoc { .. }
        | Instruction::Comment(_) => None,
    };
    (definition, uses)
}

impl FunctionLiveness {
    pub(crate) fn analyze(function: &Function) -> Self {
        let count = function.blocks.len();
        let indices: HashMap<_, _> = function.blocks.iter().enumerate()
            .map(|(index, block)| (block.id, index)).collect();
        let instruction_operands: Vec<Vec<_>> = function.blocks.iter()
            .map(|block| block.instructions.iter().map(operands).collect()).collect();
        let mut successors = vec![BTreeSet::new(); count];
        let mut block_uses = vec![BTreeSet::new(); count];
        let mut block_defs = vec![BTreeSet::new(); count];
        for (index, block) in function.blocks.iter().enumerate() {
            // Some IR producers do not fill successor metadata. Read actual
            // branches too, and conservatively retain explicitly recorded edges.
            let mut labels = block.successors.clone();
            let mut terminated = false;
            for instruction in &block.instructions {
                match instruction {
                    Instruction::Branch(label) => { labels.push(*label); terminated = true; }
                    Instruction::BranchCond { true_label, false_label, .. } => {
                        labels.extend([*true_label, *false_label]);
                        terminated = true;
                    }
                    Instruction::Return(_) => terminated = true,
                    _ => {}
                }
            }
            for label in labels {
                if let Some(&target) = indices.get(&label) {
                    successors[index].insert(target);
                }
            }
            if !terminated && index + 1 < count {
                successors[index].insert(index + 1);
            }
            for (definition, uses) in &instruction_operands[index] {
                block_uses[index].extend(uses.difference(&block_defs[index]).copied());
                if let Some(definition) = definition { block_defs[index].insert(*definition); }
            }
        }
        let mut live_in = vec![BTreeSet::new(); count];
        let mut live_out = vec![BTreeSet::new(); count];
        loop {
            let mut changed = false;
            for index in (0..count).rev() {
                let out: BTreeSet<_> = successors[index].iter()
                    .flat_map(|&successor| live_in[successor].iter().copied()).collect();
                let input: BTreeSet<_> = block_uses[index].iter().copied()
                    .chain(out.difference(&block_defs[index]).copied()).collect();
                changed |= out != live_out[index] || input != live_in[index];
                live_out[index] = out;
                live_in[index] = input;
            }
            if !changed { break; }
        }
        let mut result = Self::default();
        for (index, instructions) in instruction_operands.iter().enumerate() {
            let mut live = live_out[index].clone();
            let mut before = vec![BTreeSet::new(); instructions.len()];
            let mut after = before.clone();
            for position in (0..instructions.len()).rev() {
                after[position] = live.clone();
                let (definition, uses) = &instructions[position];
                if let Some(definition) = definition { live.remove(definition); }
                live.extend(uses);
                before[position] = live.clone();
            }
            result.before.push(before);
            result.after.push(after);
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_frontend::ir::{AsmOperandIR, BasicBlock, FatPointer, IrBinaryOp, IrType};
    use rcc_frontend::BankTag;

    fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
        let mut block = BasicBlock::new(id);
        block.instructions = instructions;
        block
    }

    #[test]
    fn follows_diamond_and_loop_edges_without_successor_metadata() {
        let mut function = Function::new("liveness".into(), IrType::I16);
        function.blocks = vec![
            block(0, vec![Instruction::BranchCond {
                condition: Value::Temp(1), true_label: 10, false_label: 20,
            }]),
            block(10, vec![Instruction::Return(Some(Value::Temp(2)))]),
            block(20, vec![Instruction::BranchCond {
                condition: Value::Temp(3), true_label: 20, false_label: 30,
            }]),
            block(30, vec![Instruction::Return(Some(Value::Temp(4)))]),
        ];
        let live = FunctionLiveness::analyze(&function);
        assert_eq!(live.before[0][0], BTreeSet::from([1, 2, 3, 4]));
        assert_eq!(live.after[0][0], BTreeSet::from([2, 3, 4]));
        assert_eq!(live.before[1][0], BTreeSet::from([2]));
        assert!(live.after[1][0].is_empty());
        assert_eq!(live.after[2][0], BTreeSet::from([3, 4]));
    }

    #[test]
    fn tracks_indirect_call_nested_pointers_and_inline_assembly_outputs() {
        let pointer = Value::FatPtr(FatPointer {
            addr: Box::new(Value::AddressOffset { base: Box::new(Value::Temp(5)), cells: 3 }),
            bank: BankTag::Mixed,
        });
        let mut function = Function::new("nested".into(), IrType::I16);
        function.blocks.push(block(0, vec![
            Instruction::Call {
                result: Some(7), function: pointer, args: vec![Value::Temp(6)],
                result_type: IrType::I16, stack_args_from: None,
            },
            Instruction::InlineAsm {
                assembly: String::new(), inputs: vec![], clobbers: vec![],
                outputs: vec![AsmOperandIR {
                    constraint: "+r".into(), value: Value::Temp(7), tied_to: None,
                }],
            },
            Instruction::Return(None),
        ]));
        let live = FunctionLiveness::analyze(&function);
        assert_eq!(live.before[0][0], BTreeSet::from([5, 6]));
        assert_eq!(live.after[0][0], BTreeSet::from([7]));
        assert_eq!(live.before[0][1], BTreeSet::from([7]));
        assert!(live.after[0][1].is_empty());
    }

    #[test]
    fn preserves_preheader_value_used_in_loop_and_releases_dead_results() {
        let mut function = Function::new("loop".into(), IrType::I16);
        function.blocks = vec![
            block(0, vec![
                Instruction::Binary { result: 2, op: IrBinaryOp::Add,
                    lhs: Value::Temp(1), rhs: Value::Constant(7), result_type: IrType::I16 },
                Instruction::Branch(1),
            ]),
            block(1, vec![
                Instruction::Binary { result: 3, op: IrBinaryOp::Add,
                    lhs: Value::Temp(2), rhs: Value::Constant(4), result_type: IrType::I16 },
                Instruction::BranchCond { condition: Value::Temp(4), true_label: 1, false_label: 2 },
            ]),
            block(2, vec![Instruction::Return(Some(Value::Temp(2)))]),
        ];
        let live = FunctionLiveness::analyze(&function);
        assert_eq!(live.before[0][0], BTreeSet::from([1, 4]));
        assert_eq!(live.after[0][0], BTreeSet::from([2, 4]));
        assert_eq!(live.after[1][0], BTreeSet::from([2, 4]));
        assert!(!live.after[1][0].contains(&3));
    }
}
