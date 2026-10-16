use super::*;
use rcc_ir::ir::{BasicBlock, IrBinaryOp as B, IrType as T};

fn masked_pattern(ty: T) -> Function {
    let mut f = Function::new("pattern".into(), ty.clone());
    f.add_parameter(0, ty.clone());
    let mut b = BasicBlock::new(0);
    for (result, op, lhs, rhs) in [
        (1, B::Mul, Value::Temp(0), Value::Constant(37)),
        (2, B::Add, Value::Temp(1), Value::Constant(11)),
        (3, B::And, Value::Temp(2), Value::Constant(255)),
    ] {
        b.instructions.push(Instruction::Binary {
            result,
            op,
            lhs,
            rhs,
            result_type: ty.clone(),
        });
    }
    b.instructions
        .push(Instruction::Return(Some(Value::Temp(3))));
    f.blocks.push(b);
    f
}

#[test]
fn low_bits_mask_removes_wide_pattern_multiply_but_keeps_result_abi() {
    for ty in [T::I32, T::I64] {
        let mut f = masked_pattern(ty.clone());
        optimize_function(&mut f);
        assert!(
            !f.blocks[0].instructions.iter().any(|i| matches!(
                i,
                Instruction::Binary {
                    op: B::Mul | B::Add,
                    result_type: T::I32 | T::I64,
                    ..
                }
            )),
            "unused high-word arithmetic remains: {:?}",
            f.blocks[0].instructions
        );
        assert!(f.blocks[0].instructions.iter().any(|i| matches!(
            i,
            Instruction::Binary {
                op: B::Mul,
                result_type: T::I16,
                ..
            }
        )));
        assert!(f.blocks[0].instructions.iter().any(|i| matches!(i,
            Instruction::Unary { result: 3, op: rcc_ir::ir::IrUnaryOp::ZExt, result_type, .. } if *result_type == ty)));
    }
}

fn evaluate(f: &Function, input: u64) -> u64 {
    let mut values = HashMap::from([(0, input)]);
    let get = |v: &Value, values: &HashMap<u32, u64>| match v {
        Value::Constant(c) => *c as u64,
        Value::Temp(id) => values[id],
        _ => panic!("unexpected test operand"),
    };
    let mask = |ty: &T| match ty {
        T::I16 => 65535,
        T::I32 => 0xffffffff,
        T::I64 => u64::MAX,
        _ => panic!("unexpected type"),
    };
    for inst in &f.blocks[0].instructions {
        let (id, value, ty) = match inst {
            Instruction::Binary {
                result,
                op,
                lhs,
                rhs,
                result_type,
            } => {
                let (a, b) = (get(lhs, &values), get(rhs, &values));
                let v = match op {
                    B::Add => a.wrapping_add(b),
                    B::Sub => a.wrapping_sub(b),
                    B::Mul => a.wrapping_mul(b),
                    B::And => a & b,
                    B::Or => a | b,
                    B::Xor => a ^ b,
                    B::UDiv => a / b,
                    B::URem => a % b,
                    B::LShr => a >> b,
                    B::Shl => a << b,
                    _ => panic!("unexpected test operation"),
                };
                (*result, v, result_type)
            }
            Instruction::Unary {
                result,
                operand,
                result_type,
                ..
            } => (*result, get(operand, &values), result_type),
            Instruction::Return(Some(v)) => return get(v, &values),
            _ => panic!("unexpected test instruction: {inst:?}"),
        };
        values.insert(id, value & mask(ty));
    }
    panic!("missing return")
}

#[test]
fn low_bits_preserves_modular_carries_and_every_kind_of_word_mask() {
    for ty in [T::I32, T::I64] {
        for op in [B::Add, B::Sub, B::Mul, B::And, B::Or, B::Xor, B::Shl] {
            for mask in [0, 1, 255, 32768, 0x8001, 65535] {
                let mut original = masked_pattern(ty.clone());
                if let Instruction::Binary {
                    op: operation, rhs, ..
                } = &mut original.blocks[0].instructions[0]
                {
                    *operation = op;
                    *rhs = Value::Constant(if op == B::Shl { 15 } else { 37 });
                }
                if let Instruction::Binary { rhs, .. } = &mut original.blocks[0].instructions[2] {
                    *rhs = Value::Constant(mask);
                }
                let mut optimized = original.clone();
                optimize_function(&mut optimized);
                let mut input = u64::MAX;
                for _ in 0..1000 {
                    if ty == T::I32 {
                        input &= 0xffffffff;
                    }
                    assert_eq!(
                        evaluate(&original, input),
                        evaluate(&optimized, input),
                        "{ty:?} {op:?} mask {mask}"
                    );
                    input = input.wrapping_mul(6364136223846793005).wrapping_add(1);
                }
            }
        }
    }
}

#[test]
fn low_bits_does_not_narrow_division_right_shifts_or_large_left_shifts() {
    for ty in [T::I32, T::I64] {
        for (op, count) in [
            (B::UDiv, 37),
            (B::URem, 37),
            (B::LShr, 17),
            (B::Shl, 16),
            (B::Shl, 31),
        ] {
            let mut original = masked_pattern(ty.clone());
            if let Instruction::Binary {
                op: operation, rhs, ..
            } = &mut original.blocks[0].instructions[0]
            {
                *operation = op;
                *rhs = Value::Constant(count);
            }
            let mut optimized = original.clone();
            optimize_function(&mut optimized);
            assert!(optimized.blocks[0].instructions.iter().any(|i| matches!(i,
                Instruction::Binary { op: operation, result_type, .. } if *operation == op && *result_type == ty)));
            for input in [0, 1, 65535, 65536, 0x12345678, 0xffffffff] {
                assert_eq!(
                    evaluate(&original, input),
                    evaluate(&optimized, input),
                    "{ty:?} {op:?}"
                );
            }
        }
    }
}

#[test]
fn low_bits_keeps_shared_wide_values_and_wide_masks() {
    let mut f = masked_pattern(T::I32);
    f.blocks[0].instructions.insert(
        2,
        Instruction::Store {
            value: Value::Temp(1),
            ptr: Value::Global("wide_product".into()),
            value_type: T::I32,
            volatile: true,
        },
    );
    optimize_function(&mut f);
    assert!(f.blocks[0].instructions.iter().any(|i| matches!(
        i,
        Instruction::Binary {
            result: 1,
            op: B::Mul,
            result_type: T::I32,
            ..
        }
    )));
    assert!(!f.blocks[0].instructions.iter().any(|i| matches!(
        i,
        Instruction::Binary {
            op: B::Mul,
            result_type: T::I16,
            ..
        }
    )));
    for mask in [65536, 0x80000000, -1] {
        let mut f = masked_pattern(T::I32);
        if let Instruction::Binary { rhs, .. } = &mut f.blocks[0].instructions[2] {
            *rhs = Value::Constant(mask);
        }
        optimize_function(&mut f);
        assert!(f.blocks[0].instructions.iter().any(|i| matches!(
            i,
            Instruction::Binary {
                op: B::Mul,
                result_type: T::I32,
                ..
            }
        )));
    }
}

#[test]
fn low_bits_does_not_duplicate_volatile_reads_or_calls() {
    for input in [
        Instruction::Load {
            result: 4,
            ptr: Value::Global("port".into()),
            result_type: T::I32,
            volatile: true,
        },
        Instruction::Call {
            result: Some(4),
            function: Value::Global("input".into()),
            args: vec![],
            result_type: T::I32,
            stack_args_from: None,
        },
    ] {
        let mut f = masked_pattern(T::I32);
        if let Instruction::Binary { lhs, .. } = &mut f.blocks[0].instructions[0] {
            *lhs = Value::Temp(4);
        }
        f.blocks[0].instructions.insert(0, input.clone());
        optimize_function(&mut f);
        assert_eq!(
            f.blocks[0]
                .instructions
                .iter()
                .filter(|i| **i == input)
                .count(),
            1
        );
        assert_eq!(f.blocks[0].instructions[0], input);
    }
}
