use super::*;
use rcc_frontend::ir::{BasicBlock, IrBinaryOp as B, IrType as T, IrUnaryOp as U};

fn binary(result: u32, op: B, lhs: Value, rhs: Value, ty: T) -> Instruction {
    Instruction::Binary {
        result,
        op,
        lhs,
        rhs,
        result_type: ty,
    }
}
fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
    let mut b = BasicBlock::new(id);
    b.instructions = instructions;
    b
}

#[test]
fn gep_keeps_native_zero_extended_indices_but_not_signed_or_wide_ones() {
    for (source_type, extension, expected) in [
        (T::I16, U::ZExt, 0),
        (T::I8, U::ZExt, 0),
        (T::I16, U::SExt, 1),
        (T::I32, U::ZExt, 1),
    ] {
        let mut f = Function::new("index".into(), T::FatPtr(Box::new(T::I16)));
        f.add_parameter(0, source_type);
        f.blocks.push(block(0, vec![
            Instruction::Unary { result: 1, op: extension, operand: Value::Temp(0), result_type: T::I32 },
            Instruction::GetElementPtr {
                result: 2,
                ptr: Value::FatPtr(rcc_frontend::ir::FatPointer {
                    addr: Box::new(Value::Constant(1234)), bank: rcc_frontend::BankTag::Global,
                }),
                indices: vec![Value::Temp(1)],
                result_type: T::FatPtr(Box::new(T::I16)),
                source_element_type: T::I16,
                is_struct_field: false,
            },
            Instruction::Return(Some(Value::Temp(2))),
        ]));
        optimize_function(&mut f);
        let index = f.blocks[0].instructions.iter().find_map(|i| match i {
            Instruction::GetElementPtr { indices, .. } => Some(&indices[0]),
            _ => None,
        }).unwrap();
        assert_eq!(index, &Value::Temp(expected));
    }
}

#[test]
fn constants_obey_target_width_and_signedness() {
    assert_eq!(fold_binary(B::Add, 1, 0, &T::I1), Some(1));
    assert_eq!(fold_binary(B::Add, 255, 0, &T::I8), Some(255));
    assert_eq!(fold_binary(B::Add, 65535, 2, &T::I16), Some(1));
    assert_eq!(fold_binary(B::Mul, 320, 200, &T::I32), Some(64000));
    assert_eq!(fold_binary(B::Mul, 320, 200, &T::I16), Some(-1536));
    assert_eq!(fold_binary(B::Ult, -1, 1, &T::I16), Some(0));
    assert_eq!(fold_binary(B::Slt, -1, 1, &T::I16), Some(1));
    assert_eq!(fold_binary(B::UDiv, -1, 2, &T::I32), Some(2147483647));
    assert_eq!(fold_binary(B::UDiv, -1, 2, &T::I64), Some(i64::MAX));
    assert_eq!(fold_binary(B::SDiv, -17, 4, &T::I32), Some(-4));
    assert_eq!(fold_binary(B::SRem, -17, 4, &T::I32), Some(-1));
    assert_eq!(fold_binary(B::AShr, 0x8000, 15, &T::I16), Some(-1));
    assert_eq!(fold_binary(B::LShr, -1, 15, &T::I16), Some(1));
    assert_eq!(fold_binary(B::Shl, 1, 63, &T::I64), Some(i64::MIN));
}

#[test]
fn undefined_arithmetic_is_not_folded() {
    assert_eq!(fold_binary(B::UDiv, 1, 0, &T::I32), None);
    assert_eq!(fold_binary(B::SDiv, -32768, -1, &T::I16), None);
    assert_eq!(fold_binary(B::SRem, i64::MIN, -1, &T::I64), None);
    assert_eq!(fold_binary(B::Shl, 1, 16, &T::I16), None);
    assert_eq!(fold_binary(B::LShr, 1, -1, &T::I64), None);
    assert_eq!(fold_binary(B::Mul, 1, 2, &T::F32), None);
}

#[test]
fn boolean_conversion_compares_all_words_before_narrowing() {
    for (ty, value) in [(T::I32, 65536), (T::I64, 1i64 << 48)] {
        let mut f = Function::new("bool".into(), T::I16);
        f.blocks.push(block(
            0,
            vec![
                binary(0, B::Add, Value::Constant(value), Value::Constant(0), ty),
                binary(1, B::Ne, Value::Temp(0), Value::Constant(0), T::I16),
                Instruction::Return(Some(Value::Temp(1))),
            ],
        ));
        optimize_function(&mut f);
        assert_eq!(
            f.blocks[0].instructions[0],
            binary(1, B::Add, Value::Constant(1), Value::Constant(0), T::I16)
        );
    }
}

#[test]
fn literal_extension_does_not_guess_source_width_from_value() {
    for value in [32768, 65535] {
        let mut f = Function::new("extend_long".into(), T::I64);
        let extend = Instruction::Unary {
            result: 0,
            op: U::SExt,
            operand: Value::Constant(value),
            result_type: T::I64,
        };
        f.blocks.push(block(
            0,
            vec![extend.clone(), Instruction::Return(Some(Value::Temp(0)))],
        ));
        optimize_function(&mut f);
        assert_eq!(f.blocks[0].instructions[0], extend);
    }
}

#[test]
fn fold_propagates_extensions_without_losing_call_argument_width() {
    let mut f = Function::new("constant_argument".into(), T::I32);
    f.blocks.push(block(
        0,
        vec![
            binary(
                0,
                B::Mul,
                Value::Constant(320),
                Value::Constant(200),
                T::I16,
            ),
            Instruction::Unary {
                result: 1,
                op: U::ZExt,
                operand: Value::Temp(0),
                result_type: T::I32,
            },
            binary(2, B::Mul, Value::Temp(1), Value::Constant(2), T::I32),
            Instruction::Call {
                result: Some(3),
                function: Value::Function("take_long".into()),
                args: vec![Value::Temp(2)],
                result_type: T::I32,
                stack_args_from: None,
            },
            Instruction::Return(Some(Value::Temp(3))),
        ],
    ));
    optimize_function(&mut f);
    assert_eq!(
        f.blocks[0].instructions[0],
        binary(
            2,
            B::Add,
            Value::Constant(128000),
            Value::Constant(0),
            T::I32
        )
    );
    assert!(
        matches!(&f.blocks[0].instructions[1], Instruction::Call { args, .. } if args == &[Value::Temp(2)])
    );
}

#[test]
fn unsigned_power_of_two_operations_become_shifts_and_masks() {
    let mut f = Function::new("strength".into(), T::I32);
    f.add_parameter(0, T::I32);
    f.blocks.push(block(
        0,
        vec![
            binary(1, B::Mul, Value::Temp(0), Value::Constant(8), T::I32),
            binary(2, B::UDiv, Value::Temp(1), Value::Constant(16), T::I32),
            binary(3, B::URem, Value::Temp(2), Value::Constant(256), T::I32),
            Instruction::Return(Some(Value::Temp(3))),
        ],
    ));
    optimize_function(&mut f);
    assert!(matches!(
        f.blocks[0].instructions[0],
        Instruction::Binary {
            op: B::Shl,
            rhs: Value::Constant(3),
            ..
        }
    ));
    assert!(matches!(
        f.blocks[0].instructions[1],
        Instruction::Binary {
            op: B::LShr,
            rhs: Value::Constant(4),
            ..
        }
    ));
    assert!(matches!(
        f.blocks[0].instructions[2],
        Instruction::Binary {
            op: B::And,
            rhs: Value::Constant(255),
            ..
        }
    ));
}

fn loop_function() -> Function {
    let mut f = Function::new("loop".into(), T::I32);
    f.add_parameter(0, T::I32);
    f.add_parameter(1, T::I16);
    f.blocks = vec![
        block(0, vec![Instruction::Branch(1)]),
        block(
            1,
            vec![Instruction::BranchCond {
                condition: Value::Temp(1),
                true_label: 2,
                false_label: 3,
            }],
        ),
        block(
            2,
            vec![
                binary(2, B::Mul, Value::Temp(0), Value::Constant(7), T::I32),
                binary(3, B::Add, Value::Temp(2), Value::Constant(17), T::I32),
                Instruction::Store {
                    value: Value::Temp(3),
                    ptr: Value::Global("output".into()),
                    value_type: T::I32,
                    volatile: true,
                },
                Instruction::Branch(1),
            ],
        ),
        block(3, vec![Instruction::Return(Some(Value::Constant(0)))]),
    ];
    f
}

#[test]
fn hoists_dependent_pure_expressions_even_in_zero_trip_loops() {
    let mut f = loop_function();
    optimize_function(&mut f);
    assert!(matches!(
        f.blocks[0].instructions[0],
        Instruction::Binary { result: 2, .. }
    ));
    assert!(matches!(
        f.blocks[0].instructions[1],
        Instruction::Binary { result: 3, .. }
    ));
    assert_eq!(
        f.blocks[0].instructions.last(),
        Some(&Instruction::Branch(1))
    );
    assert!(matches!(
        f.blocks[2].instructions[0],
        Instruction::Store { volatile: true, .. }
    ));
}

#[test]
fn memory_and_potentially_trapping_division_stay_in_loop() {
    let mut f = loop_function();
    f.blocks[2].instructions = vec![
        Instruction::Load {
            result: 4,
            ptr: Value::Global("device".into()),
            result_type: T::I32,
            volatile: true,
        },
        Instruction::Load {
            result: 5,
            ptr: Value::Global("ordinary".into()),
            result_type: T::I32,
            volatile: false,
        },
        binary(2, B::UDiv, Value::Temp(0), Value::Temp(1), T::I32),
        binary(3, B::Add, Value::Temp(2), Value::Temp(4), T::I32),
        Instruction::Store {
            value: Value::Temp(3),
            ptr: Value::Global("output".into()),
            value_type: T::I32,
            volatile: false,
        },
        Instruction::Branch(1),
    ];
    optimize_function(&mut f);
    assert_eq!(f.blocks[0].instructions, vec![Instruction::Branch(1)]);
    assert_eq!(f.blocks[2].instructions.len(), 6);
}

#[test]
fn no_hoist_from_irreducible_loop_or_without_dedicated_preheader() {
    let mut f = loop_function();
    f.blocks[0].instructions = vec![Instruction::BranchCond {
        condition: Value::Temp(1),
        true_label: 1,
        false_label: 3,
    }];
    optimize_function(&mut f);
    assert_eq!(f.blocks[0].instructions.len(), 1);
    assert!(matches!(
        f.blocks[2].instructions[0],
        Instruction::Binary { result: 2, .. }
    ));
}

#[test]
fn incomplete_cfg_does_not_authorize_hoisting() {
    let mut f = loop_function();
    f.blocks[0].instructions.clear();
    optimize_function(&mut f);
    assert!(f.blocks[0].instructions.is_empty());
    assert!(matches!(
        f.blocks[2].instructions[0],
        Instruction::Binary { result: 2, .. }
    ));
}

fn local_invariant(volatile: bool) -> Function {
    let mut f = loop_function();
    f.blocks[0].instructions = vec![
        Instruction::Alloca {
            result: 10,
            alloc_type: T::I32,
            count: None,
            result_type: T::FatPtr(Box::new(T::I32)),
        },
        Instruction::Store {
            value: Value::Temp(0),
            ptr: Value::Temp(10),
            value_type: T::I32,
            volatile,
        },
        Instruction::Branch(1),
    ];
    f.blocks[2].instructions.insert(
        0,
        Instruction::Load {
            result: 11,
            ptr: Value::Temp(10),
            result_type: T::I32,
            volatile,
        },
    );
    if let Instruction::Binary { lhs, .. } = &mut f.blocks[2].instructions[1] {
        *lhs = Value::Temp(11);
    }
    f
}

#[test]
fn promotes_non_escaping_single_store_local_and_hoists_its_expression() {
    let mut f = local_invariant(false);
    optimize_function(&mut f);
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, Instruction::Alloca { .. } | Instruction::Load { .. })));
    assert!(matches!(
        f.blocks[0].instructions[0],
        Instruction::Binary { result: 11, .. }
    ));
    assert_eq!(f.blocks[2].instructions.len(), 2); // store output and backedge
}

#[test]
fn retains_volatile_or_escaping_local_memory() {
    let mut volatile = local_invariant(true);
    optimize_function(&mut volatile);
    assert!(matches!(
        volatile.blocks[2].instructions[0],
        Instruction::Load { volatile: true, .. }
    ));
    let mut escaped = local_invariant(false);
    escaped.blocks[2].instructions.insert(
        0,
        Instruction::Call {
            result: None,
            function: Value::Function("mutate".into()),
            args: vec![Value::Temp(10)],
            result_type: T::Void,
            stack_args_from: None,
        },
    );
    optimize_function(&mut escaped);
    assert!(matches!(
        escaped.blocks[2].instructions[1],
        Instruction::Load { .. }
    ));
}

#[test]
fn store_must_dominate_every_local_read() {
    let mut f = local_invariant(false);
    let store = f.blocks[0].instructions.remove(1);
    // The first read occurs before the store on the first iteration.
    f.blocks[2].instructions.insert(1, store);
    optimize_function(&mut f);
    assert!(matches!(
        f.blocks[2].instructions[0],
        Instruction::Load { .. }
    ));
    assert!(matches!(
        f.blocks[0].instructions[0],
        Instruction::Alloca { .. }
    ));
}
