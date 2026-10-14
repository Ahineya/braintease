//! Preserve proved pointer banks without changing the pointer ABI. Facts refer
//! only to SSA values: memory loads and unknown control-flow joins stay unknown.
//! A same-bank GEP needs an unsigned range proof; no wraparound is assumed away.
use rcc_common::TempId;
use rcc_ir::{
    ir::{FatPointer, Function, Instruction as I, IrBinaryOp as B, IrType, IrUnaryOp as U, Value},
    BankTag,
};
use std::collections::HashMap;

type Range = (u64, u64);

fn max_value(ty: &IrType) -> Option<u64> {
    Some(match ty {
        IrType::I1 => 1,
        IrType::I8 => 255,
        IrType::I16 => 65535,
        IrType::I32 => u32::MAX as u64,
        IrType::I64 => u64::MAX,
        _ => return None,
    })
}

struct Facts<'a> {
    definitions: HashMap<TempId, &'a I>,
    types: HashMap<TempId, IrType>,
    constants: HashMap<TempId, i64>,
    guards: Vec<HashMap<TempId, Range>>,
}

impl Facts<'_> {
    fn constant(&self, value: &Value) -> Option<i64> {
        match value {
            Value::Constant(c) => Some(*c),
            Value::Temp(t) => self.constants.get(t).copied(),
            _ => None,
        }
    }

    fn range(&self, value: &Value, block: usize, depth: usize) -> Option<Range> {
        if let Some(c) = self.constant(value) {
            let mask = match value {
                Value::Temp(t) => self.types.get(t).and_then(max_value)?,
                _ => u32::MAX as u64,
            };
            let c = c as u64 & mask;
            return Some((c, c));
        }
        let Value::Temp(id) = value else {
            return None;
        };
        let limit = max_value(self.types.get(id)?)?;
        let full = (0, limit);
        let inferred = if depth >= 24 {
            full
        } else {
            let operand = |v| self.range(v, block, depth + 1);
            match self.definitions.get(id).copied() {
                Some(I::Unary {
                    op: U::ZExt,
                    operand: v,
                    ..
                }) => operand(v).filter(|r| r.1 <= limit).unwrap_or(full),
                Some(I::Unary {
                    op: U::SExt,
                    operand: v,
                    ..
                }) => {
                    let source_max = match v {
                        Value::Temp(t) => self.types.get(t).and_then(max_value),
                        _ => None,
                    };
                    operand(v)
                        .filter(|r| source_max.is_some_and(|m| r.1 <= m / 2) && r.1 <= limit)
                        .unwrap_or(full)
                }
                Some(I::Unary {
                    op: U::Trunc,
                    operand: v,
                    ..
                })
                | Some(I::Cast { value: v, .. }) => {
                    operand(v).filter(|r| r.1 <= limit).unwrap_or(full)
                }
                Some(I::Binary { op, lhs, rhs, .. }) => match (operand(lhs), operand(rhs)) {
                    (Some(a), Some(b)) => {
                        let r = match op {
                            B::Add => a.0.checked_add(b.0).zip(a.1.checked_add(b.1)),
                            B::Sub if a.0 >= b.1 => Some((a.0 - b.1, a.1 - b.0)),
                            B::Mul => a.0.checked_mul(b.0).zip(a.1.checked_mul(b.1)),
                            B::And if b.0 == b.1 => Some((0, a.1.min(b.1))),
                            _ => None,
                        };
                        r.filter(|r| r.1 <= limit).unwrap_or(full)
                    }
                    _ => full,
                },
                _ => full,
            }
        };
        let guarded = self.guards[block].get(id).copied().unwrap_or(full);
        let result = (inferred.0.max(guarded.0), inferred.1.min(guarded.1));
        (result.0 <= result.1).then_some(result)
    }
}

fn guarded_upper(
    condition: &Value,
    defs: &HashMap<TempId, &I>,
    constants: &HashMap<TempId, i64>,
    depth: usize,
) -> Option<(TempId, u64)> {
    if depth >= 12 {
        return None;
    }
    let Value::Temp(id) = condition else {
        return None;
    };
    let constant = |v: &Value| match v {
        Value::Constant(c) => Some(*c),
        Value::Temp(t) => constants.get(t).copied(),
        _ => None,
    };
    match defs.get(id).copied()? {
        I::Binary {
            op: B::Ne,
            lhs,
            rhs,
            ..
        } if constant(rhs) == Some(0) => guarded_upper(lhs, defs, constants, depth + 1),
        I::Cast { value, .. } => guarded_upper(value, defs, constants, depth + 1),
        I::Binary {
            op: B::Ult,
            lhs: Value::Temp(t),
            rhs,
            ..
        } => {
            let bound = constant(rhs)?;
            (bound > 0).then_some((*t, bound as u64 - 1))
        }
        _ => None,
    }
}

/// Range guards apply only beneath a uniquely entered true successor. Its
/// dominance then proves that the comparison was true on every incoming path.
fn guards(
    function: &Function,
    definitions: &HashMap<TempId, &I>,
    constants: &HashMap<TempId, i64>,
) -> Vec<HashMap<TempId, Range>> {
    let n = function.blocks.len();
    let mut result = vec![HashMap::new(); n];
    if n == 0 || function.blocks.iter().any(|b| !b.has_terminator()) {
        return result;
    }
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let Ok(cfg) = super::cfg::Cfg::new(function) else {
        return result;
    };
    for (i, b) in function.blocks.iter().enumerate() {
        let Some(I::BranchCond {
            condition,
            true_label,
            false_label,
        }) = b.instructions.last()
        else {
            continue;
        };
        if true_label == false_label {
            continue;
        }
        let target = ids[true_label];
        if target == 0 || cfg.predecessors[target] != [i] {
            continue;
        }
        let Some((value, high)) = guarded_upper(condition, definitions, constants, 0) else {
            continue;
        };
        for j in 0..n {
            if cfg.reachable.contains(&j) && cfg.dominators[j].contains(&target) {
                result[j]
                    .entry(value)
                    .and_modify(|r: &mut Range| r.1 = r.1.min(high))
                    .or_insert((0, high));
            }
        }
    }
    result
}

fn known(value: &Value, pointers: &HashMap<TempId, FatPointer>) -> Option<FatPointer> {
    match value {
        Value::Temp(t) => pointers.get(t).cloned(),
        Value::FatPtr(p) if p.bank == BankTag::Mixed => known(&p.addr, pointers),
        Value::FatPtr(p) => Some(p.clone()),
        Value::Constant(0) => Some(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        }),
        _ => None,
    }
}

fn substitute(value: &mut Value, pointers: &HashMap<TempId, FatPointer>) {
    match value {
        Value::Temp(t) => {
            if let Some(p) = pointers.get(t) {
                *value = Value::FatPtr(p.clone());
            }
        }
        Value::FatPtr(p) if p.bank == BankTag::Mixed => {
            if let Some(q) = known(&p.addr, pointers) {
                *p = q;
            }
        }
        Value::ConstantAggregate(values) => {
            for v in values {
                substitute(v, pointers);
            }
        }
        // An explicit bank's address is a scalar, not a pointer operand.
        _ => {}
    }
}

fn substitute_operands(inst: &mut I, pointers: &HashMap<TempId, FatPointer>) {
    let sub = |v: &mut Value| substitute(v, pointers);
    match inst {
        I::Binary { lhs, rhs, .. } => {
            sub(lhs);
            sub(rhs);
        }
        I::Unary { operand, .. } => sub(operand),
        I::Load { ptr, .. } => sub(ptr),
        I::Store { value, ptr, .. } => {
            sub(value);
            sub(ptr);
        }
        I::GetElementPtr { ptr, indices, .. } => {
            sub(ptr);
            for v in indices {
                sub(v);
            }
        }
        I::Alloca { count, .. } | I::Return(count) => {
            if let Some(v) = count {
                sub(v);
            }
        }
        I::Call { function, args, .. } => {
            sub(function);
            for v in args {
                sub(v);
            }
        }
        I::Intrinsic { args, .. } => {
            for v in args {
                sub(v);
            }
        }
        I::BranchCond { condition, .. } => sub(condition),
        I::Phi { incoming, .. } => {
            for (v, _) in incoming {
                sub(v);
            }
        }
        I::Cast { value, .. } => sub(value),
        I::Select {
            condition,
            true_value,
            false_value,
            ..
        } => {
            sub(condition);
            sub(true_value);
            sub(false_value);
        }
        I::InlineAsm {
            outputs, inputs, ..
        } => {
            for o in outputs.iter_mut().chain(inputs) {
                sub(&mut o.value);
            }
        }
        _ => {}
    }
}

/// Run after SSA promotion and before phi destruction. Pointer definitions are
/// replaced only when every use can carry an explicit bank. Offset arithmetic
/// is narrowed only after proving the complete result fits in the same bank.
pub fn preserve_pointer_banks(function: &mut Function, bank_size: u16) {
    if function.is_external || bank_size == 0 {
        return;
    }
    let (constants, types) = super::constants::integer_facts(function);
    let definitions: HashMap<_, _> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| super::result(i).map(|id| (id, i)))
        .collect();
    let facts = Facts {
        guards: guards(function, &definitions, &constants),
        definitions,
        types,
        constants,
    };
    let mut pointers = HashMap::new();
    let mut replacements: HashMap<TempId, Vec<I>> = HashMap::new();
    // Inline assembly outputs are definitions too, although they do not have
    // an Instruction::result. Include all referenced IDs before adding temps.
    let Some(mut next) = super::cfg::next_temp(function) else {
        return;
    };
    for _ in 0..function
        .blocks
        .iter()
        .map(|b| b.instructions.len())
        .sum::<usize>()
        .max(1)
    {
        let before = pointers.len();
        for (bi, block) in function.blocks.iter().enumerate() {
            for inst in &block.instructions {
                let Some(id) = super::result(inst) else {
                    continue;
                };
                if pointers.contains_key(&id) {
                    continue;
                }
                match inst {
                    I::Unary {
                        op: U::IntToPtr,
                        operand,
                        ..
                    } => {
                        let Some((lo, hi)) = facts.range(operand, bi, 0) else {
                            continue;
                        };
                        if hi > u32::MAX as u64 || lo >> 16 != hi >> 16 {
                            continue;
                        }
                        let bank = (lo >> 16) as u16;
                        // The three reserved high-word encodings must keep their
                        // existing runtime tag semantics. Zero remains distinct.
                        if bank >= 65533 || (lo == 0 && hi != 0) {
                            continue;
                        }
                        let tag = if hi == 0 {
                            BankTag::Null
                        } else {
                            BankTag::Heap(bank)
                        };
                        let addr = if lo == hi {
                            Value::Constant((lo & 65535) as i64)
                        } else {
                            Value::Temp(id)
                        };
                        let code = if lo == hi {
                            vec![]
                        } else {
                            vec![I::Unary {
                                result: id,
                                op: U::Trunc,
                                operand: operand.clone(),
                                result_type: IrType::I16,
                            }]
                        };
                        pointers.insert(
                            id,
                            FatPointer {
                                addr: Box::new(addr),
                                bank: tag,
                            },
                        );
                        replacements.insert(id, code);
                    }
                    I::Cast {
                        value, target_type, ..
                    } if target_type.is_pointer() => {
                        if let Some(p) = known(value, &pointers) {
                            pointers.insert(id, p);
                            replacements.insert(id, vec![]);
                        }
                    }
                    I::GetElementPtr {
                        ptr,
                        indices,
                        source_element_type,
                        is_struct_field,
                        ..
                    } if indices.len() == 1 => {
                        let Some(base) = known(ptr, &pointers) else {
                            continue;
                        };
                        let BankTag::Heap(bank) = base.bank else {
                            continue;
                        };
                        let Some((alo, ahi)) = facts.range(&base.addr, bi, 0) else {
                            continue;
                        };
                        let Some((ilo, ihi)) = facts.range(&indices[0], bi, 0) else {
                            continue;
                        };
                        let size = if *is_struct_field {
                            1
                        } else {
                            source_element_type.size_in_words().unwrap_or(1).max(1)
                        };
                        let Some(low) = ilo.checked_mul(size).and_then(|x| x.checked_add(alo))
                        else {
                            continue;
                        };
                        let Some(high) = ihi.checked_mul(size).and_then(|x| x.checked_add(ahi))
                        else {
                            continue;
                        };
                        if high >= u64::from(bank_size) {
                            continue;
                        }
                        let mut code = Vec::new();
                        let addr = if low == high {
                            Value::Constant(low as i64)
                        } else {
                            // Do not introduce a register copy for an index
                            // that already occupies one native word. A wide
                            // source still needs its explicit narrowing.
                            let index = match &indices[0] {
                                Value::Temp(t)
                                    if facts
                                        .types
                                        .get(t)
                                        .and_then(max_value)
                                        .is_some_and(|max| max <= 65535) =>
                                {
                                    *t
                                }
                                _ => {
                                    let t = next;
                                    let Some(after) = next.checked_add(1) else {
                                        return;
                                    };
                                    next = after;
                                    code.push(I::Unary {
                                        result: t,
                                        op: U::Trunc,
                                        operand: indices[0].clone(),
                                        result_type: IrType::I16,
                                    });
                                    t
                                }
                            };
                            let scaled = if size == 1 {
                                index
                            } else {
                                let t = next;
                                let Some(after) = next.checked_add(1) else {
                                    return;
                                };
                                next = after;
                                code.push(I::Binary {
                                    result: t,
                                    op: B::Mul,
                                    lhs: Value::Temp(index),
                                    rhs: Value::Constant(size as i64),
                                    result_type: IrType::I16,
                                });
                                t
                            };
                            code.push(I::Binary {
                                result: id,
                                op: B::Add,
                                lhs: (*base.addr).clone(),
                                rhs: Value::Temp(scaled),
                                result_type: IrType::I16,
                            });
                            Value::Temp(id)
                        };
                        pointers.insert(
                            id,
                            FatPointer {
                                addr: Box::new(addr),
                                bank: BankTag::Heap(bank),
                            },
                        );
                        replacements.insert(id, code);
                    }
                    _ => {}
                }
            }
        }
        if before == pointers.len() {
            break;
        }
    }
    for block in &mut function.blocks {
        let mut code = Vec::new();
        for mut inst in std::mem::take(&mut block.instructions) {
            if let Some(replacement) = super::result(&inst).and_then(|id| replacements.get(&id)) {
                // These are scalar address computations. Substitution would
                // incorrectly turn their already-extracted offsets into pointers.
                code.extend(replacement.clone());
            } else {
                substitute_operands(&mut inst, &pointers);
                code.push(inst);
            }
        }
        block.instructions = code;
    }
    super::eliminate_dead_expressions(function);
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::BasicBlock;

    fn pointer_type() -> IrType {
        IrType::FatPtr(Box::new(IrType::I16))
    }
    fn block(id: u32, instructions: Vec<I>) -> BasicBlock {
        let mut block = BasicBlock::new(id);
        block.instructions = instructions;
        block
    }
    fn int_pointer(result: TempId, value: i64) -> I {
        I::Unary {
            result,
            op: U::IntToPtr,
            operand: Value::Constant(value),
            result_type: pointer_type(),
        }
    }
    fn gep(result: TempId, base: TempId, index: Value) -> I {
        I::GetElementPtr {
            result,
            ptr: Value::Temp(base),
            indices: vec![index],
            result_type: pointer_type(),
            source_element_type: IrType::I16,
            is_struct_field: false,
        }
    }
    fn load(result: TempId, pointer: TempId) -> I {
        I::Load {
            result,
            ptr: Value::Temp(pointer),
            result_type: IrType::I16,
            volatile: true,
        }
    }
    fn guarded(base: i64, bound: i64) -> Function {
        let mut f = Function::new("guarded".into(), IrType::I16);
        f.add_parameter(0, IrType::I16);
        f.blocks = vec![
            block(
                0,
                vec![
                    int_pointer(1, base),
                    I::Binary {
                        result: 2,
                        op: B::Ult,
                        lhs: Value::Temp(0),
                        rhs: Value::Constant(bound),
                        result_type: IrType::I16,
                    },
                    I::BranchCond {
                        condition: Value::Temp(2),
                        true_label: 1,
                        false_label: 2,
                    },
                ],
            ),
            block(
                1,
                vec![
                    gep(3, 1, Value::Temp(0)),
                    load(4, 3),
                    I::Return(Some(Value::Temp(4))),
                ],
            ),
            block(2, vec![I::Return(Some(Value::Constant(0)))]),
        ];
        f
    }

    #[test]
    fn bounded_same_bank_access_carries_explicit_bank_and_scalar_offset() {
        let mut f = guarded(0x0002_0100, 8);
        preserve_pointer_banks(&mut f, 512);
        assert!(!f
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(
                i,
                I::GetElementPtr { .. }
                    | I::Unary {
                        op: U::IntToPtr,
                        ..
                    }
            )));
        assert!(f.blocks[1].instructions.iter().any(|i|matches!(i,I::Load{ptr:Value::FatPtr(p),volatile:true,..} if p.bank==BankTag::Heap(2) && *p.addr==Value::Temp(3))));
        assert!(f.blocks[1].instructions.iter().any(|i| matches!(
            i,
            I::Binary {
                result: 3,
                op: B::Add,
                lhs: Value::Constant(256),
                rhs: Value::Temp(0),
                result_type: IrType::I16,
                ..
            }
        )));
    }

    #[test]
    fn bounded_wide_index_is_still_explicitly_narrowed() {
        let mut f = guarded(256, 8);
        f.parameters[0].1 = IrType::I32;
        preserve_pointer_banks(&mut f, 512);
        assert!(f.blocks[1].instructions.iter().any(|i| matches!(
            i,
            I::Unary {
                op: U::Trunc,
                operand: Value::Temp(0),
                result_type: IrType::I16,
                ..
            }
        )));
        assert!(!f.blocks[1]
            .instructions
            .iter()
            .any(|i| matches!(i, I::GetElementPtr { .. })));
    }

    #[test]
    fn generated_offsets_do_not_reuse_inline_assembly_output_ids() {
        let mut f = guarded(256, 8);
        f.parameters[0].1 = IrType::I32;
        f.blocks[1].instructions.insert(
            2,
            I::InlineAsm {
                assembly: "LI %0, 17".into(),
                outputs: vec![rcc_ir::ir::AsmOperandIR {
                    constraint: "=r".into(),
                    value: Value::Temp(5),
                    tied_to: None,
                }],
                inputs: vec![],
                clobbers: vec![],
            },
        );
        f.blocks[1].instructions.push(I::Store {
            value: Value::Temp(5),
            ptr: Value::Global("observed".into()),
            value_type: IrType::I16,
            volatile: true,
        });
        // Move the return back after the store so the range guard is valid.
        let ret = f.blocks[1].instructions.remove(3);
        f.blocks[1].instructions.push(ret);
        preserve_pointer_banks(&mut f, 512);
        assert!(f.blocks[1].instructions.iter().any(|i| matches!(i,
            I::Unary { op: U::Trunc, result, .. } if *result > 5)));
    }

    #[test]
    fn crossing_or_unguarded_offsets_keep_general_gep() {
        for (base, bound) in [(509, 4), (512, 1), (256, 65536)] {
            let mut f = guarded(base, bound);
            preserve_pointer_banks(&mut f, 512);
            assert!(
                f.blocks[1]
                    .instructions
                    .iter()
                    .any(|i| matches!(i, I::GetElementPtr { .. })),
                "base {base} bound {bound}"
            );
        }
        let mut f = guarded(256, 8);
        // The false path can also reach the access: the true comparison is no
        // longer a precondition of this block, even though its IR is nearby.
        f.blocks[2].instructions = vec![I::Branch(1)];
        preserve_pointer_banks(&mut f, 512);
        assert!(f.blocks[1]
            .instructions
            .iter()
            .any(|i| matches!(i, I::GetElementPtr { .. })));
    }

    #[test]
    fn early_branch_cannot_authorize_later_range_guard() {
        let mut f = guarded(256, 8);
        // A malformed block can arise before frontend goto cleanup. Execution
        // can bypass the comparison, so its final terminator proves nothing.
        f.blocks[0].instructions.insert(1, I::Branch(1));
        preserve_pointer_banks(&mut f, 512);
        assert!(f.blocks[1]
            .instructions
            .iter()
            .any(|i| matches!(i, I::GetElementPtr { .. })));
    }

    #[test]
    fn negative_and_multiword_offsets_keep_general_gep() {
        for offset in [-1, 65536, 0x7fff_ffff] {
            let mut f = Function::new("offset".into(), pointer_type());
            f.blocks = vec![block(
                0,
                vec![
                    int_pointer(0, 0x0002_0100),
                    gep(1, 0, Value::Constant(offset)),
                    I::Return(Some(Value::Temp(1))),
                ],
            )];
            preserve_pointer_banks(&mut f, 512);
            assert!(
                f.blocks[0]
                    .instructions
                    .iter()
                    .any(|i| matches!(i, I::GetElementPtr { .. })),
                "offset {offset}"
            );
        }
    }

    #[test]
    fn null_and_known_banks_survive_pointer_cast_and_call() {
        for (value, bank, offset) in [
            (0, BankTag::Null, 0),
            (256, BankTag::Heap(0), 256),
            (0x0003_0000, BankTag::Heap(3), 0),
        ] {
            let mut f = Function::new("cast".into(), pointer_type());
            f.blocks = vec![block(
                0,
                vec![
                    int_pointer(0, value),
                    I::Cast {
                        result: 1,
                        value: Value::Temp(0),
                        target_type: pointer_type(),
                    },
                    I::Call {
                        result: None,
                        function: Value::Function("observe".into()),
                        args: vec![Value::Temp(1)],
                        result_type: IrType::Void,
                        stack_args_from: None,
                    },
                    I::Return(Some(Value::Temp(1))),
                ],
            )];
            preserve_pointer_banks(&mut f, 512);
            let expected = Value::FatPtr(FatPointer {
                addr: Box::new(Value::Constant(offset)),
                bank,
            });
            assert_eq!(f.blocks[0].instructions.len(), 2);
            assert!(
                matches!(&f.blocks[0].instructions[0],I::Call{args,..} if args==&vec![expected.clone()])
            );
            assert_eq!(f.blocks[0].instructions[1], I::Return(Some(expected)));
        }
    }

    #[test]
    fn runtime_zero_and_reserved_tags_are_not_claimed_as_numeric_banks() {
        for value in [0xfffd_0100, 0xfffe_0100, 0xffff_0100] {
            let mut f = Function::new("reserved".into(), pointer_type());
            f.blocks = vec![block(
                0,
                vec![int_pointer(0, value), I::Return(Some(Value::Temp(0)))],
            )];
            preserve_pointer_banks(&mut f, 512);
            assert!(matches!(
                f.blocks[0].instructions[0],
                I::Unary {
                    op: U::IntToPtr,
                    ..
                }
            ));
        }
        let mut f = Function::new("possibly_null".into(), pointer_type());
        f.add_parameter(0, IrType::I16);
        f.blocks = vec![block(
            0,
            vec![
                I::Unary {
                    result: 1,
                    op: U::IntToPtr,
                    operand: Value::Temp(0),
                    result_type: pointer_type(),
                },
                I::Return(Some(Value::Temp(1))),
            ],
        )];
        preserve_pointer_banks(&mut f, 512);
        assert!(matches!(
            f.blocks[0].instructions[0],
            I::Unary {
                op: U::IntToPtr,
                ..
            }
        ));
    }
}
