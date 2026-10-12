//! GEP already zero-extends one-word operands. Preserve that information rather
//! than hiding it behind a two-word temporary with a permanently zero high word.
use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction, IrType, IrUnaryOp, Value};
use std::collections::{HashMap, HashSet};

pub(super) fn narrow_gep_indices(function: &mut Function) {
    let mut words: HashSet<TempId> = function.parameters.iter()
        .filter(|(_, ty)| matches!(ty, IrType::I1 | IrType::I8 | IrType::I16))
        .map(|(id, _)| *id).collect();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        let (id, ty) = match inst {
            Instruction::Binary { result, result_type, .. }
            | Instruction::Unary { result, result_type, .. }
            | Instruction::Load { result, result_type, .. }
            | Instruction::Cast { result, target_type: result_type, .. }
            | Instruction::Select { result, result_type, .. }
            | Instruction::Phi { result, result_type, .. } => (*result, result_type),
            Instruction::Call { result: Some(result), result_type, .. }
            | Instruction::Intrinsic { result: Some(result), result_type, .. } => (*result, result_type),
            _ => continue,
        };
        if matches!(ty, IrType::I1 | IrType::I8 | IrType::I16) { words.insert(id); }
    }
    let zero_extended: HashMap<_, _> = function.blocks.iter().flat_map(|b| &b.instructions)
        .filter_map(|inst| match inst {
            Instruction::Unary { result, op: IrUnaryOp::ZExt, operand: Value::Temp(source), result_type: IrType::I32 }
                if words.contains(source) => Some((*result, *source)),
            _ => None,
        }).collect();
    for inst in function.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
        if let Instruction::GetElementPtr { indices, .. } = inst {
            for index in indices {
                if let Value::Temp(id) = index {
                    if let Some(source) = zero_extended.get(id) { *id = *source; }
                }
            }
        }
    }
}
