use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction, IrBinaryOp as B, IrType, IrUnaryOp as U, Value};
use std::collections::HashMap;

fn width(ty: &IrType) -> Option<u32> {
    match ty {
        IrType::I1 => Some(1),
        IrType::I8 => Some(8),
        IrType::I16 => Some(16),
        IrType::I32 => Some(32),
        IrType::I64 => Some(64),
        _ => None,
    }
}
fn mask(bits: u32) -> u64 {
    u64::MAX >> (64 - bits)
}
fn signed(value: u64, bits: u32) -> i64 {
    ((value << (64 - bits)) as i64) >> (64 - bits)
}
pub(super) fn comparison(op: B) -> bool {
    matches!(
        op,
        B::Eq | B::Ne | B::Slt | B::Sle | B::Sgt | B::Sge | B::Ult | B::Ule | B::Ugt | B::Uge
    )
}

/// Evaluate in the target's integer width, not the compiler host's width.
/// Undefined divisions and shifts are deliberately left for normal lowering.
pub fn fold_binary(op: B, lhs: i64, rhs: i64, ty: &IrType) -> Option<i64> {
    let bits = width(ty)?;
    let a = lhs as u64 & mask(bits);
    let b = rhs as u64 & mask(bits);
    let sa = signed(a, bits);
    let sb = signed(b, bits);
    let boolean = match op {
        B::Eq => Some(a == b),
        B::Ne => Some(a != b),
        B::Slt => Some(sa < sb),
        B::Sle => Some(sa <= sb),
        B::Sgt => Some(sa > sb),
        B::Sge => Some(sa >= sb),
        B::Ult => Some(a < b),
        B::Ule => Some(a <= b),
        B::Ugt => Some(a > b),
        B::Uge => Some(a >= b),
        _ => None,
    };
    if let Some(b) = boolean {
        return Some(i64::from(b));
    }
    let out = match op {
        B::Add => a.wrapping_add(b),
        B::Sub => a.wrapping_sub(b),
        B::Mul => a.wrapping_mul(b),
        B::And => a & b,
        B::Or => a | b,
        B::Xor => a ^ b,
        B::UDiv if b != 0 => a / b,
        B::URem if b != 0 => a % b,
        B::SDiv | B::SRem if sb != 0 => {
            if sa == signed(1u64 << (bits - 1), bits) && sb == -1 {
                return None;
            }
            if op == B::SDiv {
                (sa / sb) as u64
            } else {
                (sa % sb) as u64
            }
        }
        B::Shl if rhs >= 0 && (rhs as u64) < u64::from(bits) => a << rhs,
        B::LShr if rhs >= 0 && (rhs as u64) < u64::from(bits) => a >> rhs,
        B::AShr if rhs >= 0 && (rhs as u64) < u64::from(bits) => (sa >> rhs) as u64,
        _ => return None,
    };
    let out = out & mask(bits);
    Some(if bits < 16 {
        out as i64
    } else {
        signed(out, bits)
    })
}

fn constant(value: &Value, known: &HashMap<TempId, i64>) -> Option<i64> {
    match value {
        Value::Constant(c) => Some(*c),
        Value::Temp(t) => known.get(t).copied(),
        _ => None,
    }
}

fn value_width(value: &Value, types: &HashMap<TempId, IrType>) -> Option<u32> {
    match value {
        Value::Temp(t) => types.get(t).and_then(width),
        Value::Constant(c) if (-32768..=65535).contains(c) => Some(16),
        Value::Constant(c) if (i32::MIN as i64..=u32::MAX as i64).contains(c) => Some(32),
        Value::Constant(_) => Some(64),
        _ => None,
    }
}

fn evaluate(
    inst: &Instruction,
    known: &HashMap<TempId, i64>,
    types: &HashMap<TempId, IrType>,
) -> Option<(i64, IrType)> {
    match inst {
        Instruction::Binary {
            op,
            lhs,
            rhs,
            result_type,
            ..
        } => {
            let (a, b) = (constant(lhs, known)?, constant(rhs, known)?);
            // Boolean conversion declares an I16 result even when comparing
            // an I32/I64 operand. Keep the operand width until after comparing.
            let compare_type = match value_width(lhs, types)
                .unwrap_or(16)
                .max(value_width(rhs, types).unwrap_or(16))
                .max(width(result_type)?)
            {
                64 => IrType::I64,
                32 => IrType::I32,
                _ => IrType::I16,
            };
            let value = fold_binary(
                *op,
                a,
                b,
                if comparison(*op) {
                    &compare_type
                } else {
                    result_type
                },
            )?;
            Some((
                value,
                if comparison(*op) {
                    IrType::I16
                } else {
                    result_type.clone()
                },
            ))
        }
        Instruction::Unary {
            op,
            operand,
            result_type,
            ..
        } => {
            let bits = width(result_type)?;
            let value = constant(operand, known)?;
            let out = match op {
                U::Not => !value,
                U::Neg => value.wrapping_neg(),
                U::Trunc => value,
                // Literal extensions already carry their mathematical value;
                // lower_i32_extend likewise splits the literal directly. Never
                // infer a narrower signed source width for 65535L.
                U::ZExt | U::SExt if matches!(operand, Value::Constant(_)) => value,
                U::ZExt if matches!(operand, Value::Temp(_)) => {
                    (value as u64 & mask(value_width(operand, types)?)) as i64
                }
                U::SExt if matches!(operand, Value::Temp(_)) => {
                    signed(value as u64, value_width(operand, types)?)
                }
                _ => return None,
            };
            let out = out as u64 & mask(bits);
            Some((
                if bits < 16 {
                    out as i64
                } else {
                    signed(out, bits)
                },
                result_type.clone(),
            ))
        }
        _ => None,
    }
}

pub(super) fn integer_facts(function: &Function) -> (HashMap<TempId, i64>, HashMap<TempId, IrType>) {
    let mut types: HashMap<_, _> = function.parameters.iter().cloned().collect();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        let ty = match inst {
            Instruction::Binary {
                op, result_type, ..
            } => Some(if comparison(*op) {
                IrType::I16
            } else {
                result_type.clone()
            }),
            Instruction::Unary { result_type, .. }
            | Instruction::Load { result_type, .. }
            | Instruction::Call { result_type, .. }
            | Instruction::Phi { result_type, .. }
            | Instruction::Select { result_type, .. }
            | Instruction::Intrinsic { result_type, .. } => Some(result_type.clone()),
            Instruction::Cast { target_type, .. } => Some(target_type.clone()),
            _ => None,
        };
        if let (Some(id), Some(ty)) = (super::result(inst), ty) {
            types.insert(id, ty);
        }
    }
    // IR temporaries are single-assignment. A bounded fixed point also handles
    // block layouts where a dominating definition is emitted after its use.
    let mut known = HashMap::new();
    loop {
        let before = known.len();
        for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
            if let Some(id) = super::result(inst) {
                if let Some((c, _)) = evaluate(inst, &known, &types) {
                    known.insert(id, c);
                }
            }
        }
        if before == known.len() {
            break;
        }
    }
    (known, types)
}

pub(super) fn simplify(function: &mut Function) {
    let (known, types) = integer_facts(function);
    for inst in function.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
        if let Some((c, ty)) = evaluate(inst, &known, &types) {
            // Keep the typed temporary: replacing a long call argument with a
            // bare Constant would silently change its ABI to a single word.
            *inst = Instruction::Binary {
                result: super::result(inst).unwrap(),
                op: B::Add,
                lhs: Value::Constant(c),
                rhs: Value::Constant(0),
                result_type: ty,
            };
            continue;
        }
        match inst {
            Instruction::GetElementPtr { indices, .. } => {
                for index in indices {
                    if let Some(c) = constant(index, &known) {
                        *index = Value::Constant(c);
                    }
                }
            }
            Instruction::Binary {
                op,
                lhs,
                rhs,
                result_type,
                ..
            } if result_type.is_integer() => {
                // Powers of two use native shifts/masks even for multiword C
                // integers. Signed division is intentionally not rewritten.
                if *op == B::Mul
                    && constant(lhs, &known).is_some()
                    && constant(rhs, &known).is_none()
                {
                    std::mem::swap(lhs, rhs);
                }
                if let Some(c) = constant(rhs, &known) {
                    let bits = width(result_type).unwrap();
                    let unsigned = c as u64 & mask(bits);
                    if unsigned.is_power_of_two() {
                        match op {
                            B::Mul => {
                                *op = B::Shl;
                                *rhs = Value::Constant(unsigned.trailing_zeros() as i64);
                            }
                            B::UDiv => {
                                *op = B::LShr;
                                *rhs = Value::Constant(unsigned.trailing_zeros() as i64);
                            }
                            B::URem => {
                                *op = B::And;
                                *rhs = Value::Constant((unsigned - 1) as i64);
                            }
                            _ => {}
                        }
                    }
                }
            }
            _ => {}
        }
    }
    eliminate_identity_copies(function, &types);
}

/// Alias only typed SSA temporaries. Replacing a multiword copy by an untyped
/// literal would change argument width; replacing a narrowing copy would lose
/// its truncation. Neither is an algebraic identity in this IR.
fn eliminate_identity_copies(function: &mut Function, types: &HashMap<TempId, IrType>) {
    use std::collections::HashSet;
    let mut booleans: HashSet<_> = types.iter().filter_map(|(&id, ty)| (*ty == IrType::I1).then_some(id)).collect();
    let mut aliases = HashMap::<TempId, TempId>::new();
    let resolve = |mut id: TempId, aliases: &HashMap<TempId, TempId>| {
        while let Some(&next) = aliases.get(&id) { if next == id { break; } id = next; }
        id
    };
    loop {
        let before = (aliases.len(), booleans.len());
        for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
            let Some(result) = super::result(inst) else { continue; };
            let is_boolean = |value: &Value| match value {
                Value::Constant(0 | 1) => true,
                Value::Temp(id) => booleans.contains(&resolve(*id, &aliases)),
                _ => false,
            };
            let boolean = match inst {
                Instruction::Binary { op, .. } => comparison(*op),
                Instruction::Phi { incoming, .. } => !incoming.is_empty() && incoming.iter().all(|(v, _)| is_boolean(v)),
                Instruction::Select { true_value, false_value, .. } => is_boolean(true_value) && is_boolean(false_value),
                _ => false,
            };
            if boolean { booleans.insert(result); }
            let candidate = match inst {
                Instruction::Binary { op, lhs: Value::Temp(input), rhs: Value::Constant(c), result_type, .. }
                    if types.get(input) == Some(result_type) => {
                    let id = resolve(*input, &aliases);
                    let identity = (*c == 0 && matches!(op, B::Add | B::Sub | B::Or | B::Xor | B::Shl | B::LShr | B::AShr))
                        || (*c == 1 && matches!(op, B::Mul | B::UDiv | B::SDiv))
                        || (*op == B::And && width(result_type).is_some_and(|bits| *c as u64 & mask(bits) == mask(bits)))
                        || (*op == B::Ne && *c == 0 && booleans.contains(&id));
                    identity.then_some(id)
                }
                Instruction::Unary { op: U::Trunc | U::ZExt | U::SExt, operand: Value::Temp(input), result_type, .. }
                    if types.get(input) == Some(result_type) => Some(resolve(*input, &aliases)),
                _ => None,
            };
            if let Some(input) = candidate {
                if input != result { aliases.insert(result, input); }
                if booleans.contains(&input) { booleans.insert(result); }
            }
        }
        if before == (aliases.len(), booleans.len()) { break; }
    }
    if aliases.is_empty() { return; }
    fn rewrite(value: &mut Value, aliases: &HashMap<TempId, TempId>) {
        match value {
            Value::Temp(id) => { while let Some(&next) = aliases.get(id) { *id = next; } }
            Value::FatPtr(pointer) => rewrite(&mut pointer.addr, aliases),
            Value::AddressOffset { base, .. } => rewrite(base, aliases),
            Value::ConstantAggregate(values) => for value in values { rewrite(value, aliases); },
            _ => {},
        }
    }
    for block in &mut function.blocks {
        block.instructions.retain(|inst| super::result(inst).is_none_or(|id| !aliases.contains_key(&id)));
        for inst in &mut block.instructions {
            let value = |v: &mut Value| rewrite(v, &aliases);
            match inst {
                Instruction::Binary { lhs, rhs, .. } => { value(lhs); value(rhs); }
                Instruction::Unary { operand, .. } => value(operand),
                Instruction::Load { ptr, .. } => value(ptr),
                Instruction::Store { ptr, value: stored, .. } => { value(ptr); value(stored); }
                Instruction::GetElementPtr { ptr, indices, .. } => { value(ptr); for v in indices { value(v); } }
                Instruction::Alloca { count, .. } | Instruction::Return(count) => { if let Some(v) = count { value(v); } }
                Instruction::Call { function, args, .. } => { value(function); for v in args { value(v); } }
                Instruction::Intrinsic { args, .. } => for v in args { value(v); },
                Instruction::BranchCond { condition, .. } => value(condition),
                Instruction::Phi { incoming, .. } => for (v, _) in incoming { value(v); },
                Instruction::Cast { value: v, .. } => value(v),
                Instruction::Select { condition, true_value, false_value, .. } => { value(condition); value(true_value); value(false_value); }
                Instruction::InlineAsm { inputs, .. } => for input in inputs { value(&mut input.value); },
                _ => {},
            }
        }
    }
}

#[cfg(test)]
mod identity_tests {
    use super::*;
    use rcc_frontend::ir::BasicBlock;

    #[test]
    fn typed_aliases_keep_wide_arguments_and_remove_boolean_rechecks() {
        let mut f = Function::new("aliases".into(), IrType::I32);
        f.add_parameter(0, IrType::I32);
        let mut block = BasicBlock::new(0);
        block.instructions = vec![
            Instruction::Binary { result: 1, op: B::Add, lhs: Value::Temp(0), rhs: Value::Constant(0), result_type: IrType::I32 },
            Instruction::Binary { result: 2, op: B::Ne, lhs: Value::Temp(1), rhs: Value::Constant(0), result_type: IrType::I16 },
            Instruction::Binary { result: 3, op: B::Ne, lhs: Value::Temp(2), rhs: Value::Constant(0), result_type: IrType::I16 },
            Instruction::Call { result: None, function: Value::Function("observe".into()), args: vec![Value::Temp(1),Value::Temp(3)], result_type: IrType::Void, stack_args_from: None },
            Instruction::Return(Some(Value::Temp(1))),
        ];
        f.blocks.push(block);
        simplify(&mut f);
        assert_eq!(f.blocks[0].instructions.len(),3);
        assert!(matches!(&f.blocks[0].instructions[0],Instruction::Binary { result: 2,lhs: Value::Temp(0),.. }));
        assert!(matches!(&f.blocks[0].instructions[1],Instruction::Call {args,..} if args == &vec![Value::Temp(0),Value::Temp(2)]));
        assert!(matches!(&f.blocks[0].instructions[2],Instruction::Return(Some(Value::Temp(0)))));
    }

    #[test]
    fn narrowing_and_untyped_wide_constants_are_not_identity_aliases() {
        let mut f = Function::new("narrow".into(), IrType::I16);
        f.add_parameter(0, IrType::I32);
        let mut block = BasicBlock::new(0);
        block.instructions = vec![
            Instruction::Binary { result: 1, op: B::Add, lhs: Value::Temp(0), rhs: Value::Constant(0), result_type: IrType::I16 },
            Instruction::Binary { result: 2, op: B::Add, lhs: Value::Constant(1), rhs: Value::Constant(0), result_type: IrType::I32 },
            Instruction::Call { result: None,function: Value::Function("wide".into()),args: vec![Value::Temp(2)],result_type: IrType::Void,stack_args_from: None },
            Instruction::Return(Some(Value::Temp(1))),
        ];
        f.blocks.push(block);
        simplify(&mut f);
        assert_eq!(f.blocks[0].instructions.len(),4);
        assert!(matches!(&f.blocks[0].instructions[2],Instruction::Call {args,..} if args == &vec![Value::Temp(2)]));
    }
}
