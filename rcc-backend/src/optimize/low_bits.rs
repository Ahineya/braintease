//! A low-word mask makes upper arithmetic bits unobservable. Compute the
//! single-use modular expression in one target word, then restore its original
//! IR width so call arguments, stores and return values keep their ABI.
use super::{constants, operands, result, value_temps};
use rcc_common::TempId;
use rcc_ir::ir::{Function, Instruction as I, IrBinaryOp as B, IrType, IrUnaryOp as U, Value};
use std::collections::{HashMap, HashSet};

struct Narrow<'a> {
    definitions: &'a HashMap<TempId, I>,
    types: &'a HashMap<TempId, IrType>,
    uses: &'a HashMap<TempId, usize>,
    next: TempId,
    code: Vec<I>,
    changed: bool,
}
impl Narrow<'_> {
    fn emit(&mut self, instruction: impl FnOnce(TempId) -> I) -> Value {
        let id = self.next;
        self.next += 1;
        self.code.push(instruction(id));
        Value::Temp(id)
    }
    fn word(&mut self, value: &Value, depth: usize) -> Option<Value> {
        if let Value::Constant(c) = value {
            return Some(Value::Constant(*c & 65535));
        }
        let Value::Temp(id) = value else {
            return None;
        };
        if matches!(
            self.types.get(id),
            Some(IrType::I1 | IrType::I8 | IrType::I16)
        ) {
            return Some(value.clone());
        }
        if !matches!(self.types.get(id), Some(IrType::I32 | IrType::I64)) {
            return None;
        }
        // Only duplicate cheap, single-use pure expressions. Loads, calls,
        // division, right shifts and PHIs stay where they were; truncate their
        // existing result rather than speculating or changing their semantics.
        if depth < 12 && self.code.len() < 48 && self.uses.get(id) == Some(&1) {
            if let Some(I::Binary {
                op,
                lhs,
                rhs,
                result_type: IrType::I32 | IrType::I64,
                ..
            }) = self.definitions.get(id)
            {
                let (op, lhs, rhs) = (*op, lhs.clone(), rhs.clone());
                if matches!(op, B::Add | B::Sub | B::Mul | B::And | B::Or | B::Xor) {
                    let lhs = self.word(&lhs, depth + 1)?;
                    let rhs = self.word(&rhs, depth + 1)?;
                    self.changed = true;
                    return Some(self.emit(|result| I::Binary {
                        result,
                        op,
                        lhs,
                        rhs,
                        result_type: IrType::I16,
                    }));
                }
                // A native shift masks its count to 0..15; never substitute it
                // for a wide shift by 16 or more, or for a variable wide shift.
                if op == B::Shl && matches!(rhs, Value::Constant(0..=15)) {
                    let lhs = self.word(&lhs, depth + 1)?;
                    self.changed = true;
                    return Some(self.emit(|result| I::Binary {
                        result,
                        op,
                        lhs,
                        rhs,
                        result_type: IrType::I16,
                    }));
                }
            }
        }
        Some(self.emit(|result| I::Unary {
            result,
            op: U::Trunc,
            operand: value.clone(),
            result_type: IrType::I16,
        }))
    }
}

pub(super) fn narrow_masks(function: &mut Function) {
    // Most functions (especially generated initializers) contain no candidate.
    // Avoid building another definition/use graph for those large bodies.
    if !function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| {
            matches!(
                i,
                I::Binary {
                    op: B::And,
                    result_type: IrType::I32 | IrType::I64,
                    lhs: Value::Constant(0..=65535),
                    ..
                } | I::Binary {
                    op: B::And,
                    result_type: IrType::I32 | IrType::I64,
                    rhs: Value::Constant(0..=65535),
                    ..
                }
            )
        })
    {
        return;
    }
    let Some(mut next) = super::cfg::next_temp(function) else {
        return;
    };
    let types = constants::value_types(function);
    let definitions: HashMap<_, _> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| result(i).map(|id| (id, i.clone())))
        .collect();
    let mut uses = HashMap::new();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        for operand in operands(inst) {
            let mut ids = HashSet::new();
            value_temps(operand, &mut ids);
            for id in ids {
                *uses.entry(id).or_insert(0) += 1;
            }
        }
    }
    for block in &mut function.blocks {
        let mut output = Vec::new();
        for inst in std::mem::take(&mut block.instructions) {
            if let I::Binary {
                result,
                op: B::And,
                lhs,
                rhs,
                result_type: IrType::I32 | IrType::I64,
            } = &inst
            {
                let masked = match (lhs, rhs) {
                    (source, Value::Constant(mask)) | (Value::Constant(mask), source)
                        if (0..=65535).contains(mask) =>
                    {
                        Some((source, *mask))
                    }
                    _ => None,
                };
                // One expression emits at most two instructions per tree node
                // under the limits above. Leave exhausted ID spaces untouched.
                if let Some((source, mask)) = masked.filter(|_| next <= u32::MAX - 128) {
                    let mut narrow = Narrow {
                        definitions: &definitions,
                        types: &types,
                        uses: &uses,
                        next,
                        code: Vec::new(),
                        changed: false,
                    };
                    if let Some(lhs) = narrow.word(source, 0).filter(|_| narrow.changed) {
                        let word = narrow.emit(|result| I::Binary {
                            result,
                            op: B::And,
                            lhs,
                            rhs: Value::Constant(mask),
                            result_type: IrType::I16,
                        });
                        let I::Binary { result_type, .. } = &inst else {
                            unreachable!()
                        };
                        narrow.code.push(I::Unary {
                            result: *result,
                            op: U::ZExt,
                            operand: word,
                            result_type: result_type.clone(),
                        });
                        next = narrow.next;
                        output.extend(narrow.code);
                        continue;
                    }
                }
            }
            output.push(inst);
        }
        block.instructions = output;
    }
}
