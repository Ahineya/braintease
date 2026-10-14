use super::*;
use rcc_ir::ir::{AsmOperandIR, BasicBlock};
fn block(id: u32, instructions: Vec<I>) -> BasicBlock {
    let mut b = BasicBlock::new(id);
    b.instructions = instructions;
    b
}
fn load(result: u32, name: &str, volatile: bool) -> I {
    I::Load {
        result,
        ptr: Value::Global(name.into()),
        result_type: IrType::I16,
        volatile,
    }
}
fn bin(result: u32, op: B, lhs: Value, rhs: Value) -> I {
    I::Binary {
        result,
        op,
        lhs,
        rhs,
        result_type: IrType::I16,
    }
}
fn sample() -> Function {
    let mut f = Function::new("bounds".into(), IrType::I16);
    f.parameters.push((0, IrType::I16));
    f.blocks = vec![
        block(
            0,
            vec![
                load(1, "width", false),
                bin(2, B::Ult, Value::Temp(0), Value::Temp(1)),
                I::BranchCond {
                    condition: Value::Temp(2),
                    true_label: 1,
                    false_label: 3,
                },
            ],
        ),
        block(
            1,
            vec![
                load(3, "width", false),
                bin(4, B::Uge, Value::Temp(0), Value::Temp(3)),
                I::BranchCond {
                    condition: Value::Temp(4),
                    true_label: 2,
                    false_label: 3,
                },
            ],
        ),
        block(2, vec![I::Return(Some(Value::Constant(99)))]),
        block(3, vec![I::Return(Some(Value::Constant(0)))]),
    ];
    f
}
#[test]
fn readonly_loads_and_dominated_complementary_bounds_disappear() {
    let mut f = sample();
    eliminate_redundancy(&mut f);
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, I::Load { .. }))
            .count(),
        1
    );
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, I::Return(Some(Value::Constant(99))))));
}
#[test]
fn volatile_stores_calls_and_memory_clobbers_keep_distinct_reads() {
    let barriers = vec![
        I::Store {
            value: Value::Constant(3),
            ptr: Value::Global("width".into()),
            value_type: IrType::I16,
            volatile: false,
        },
        I::Store {
            value: Value::Constant(3),
            ptr: Value::Temp(0),
            value_type: IrType::I16,
            volatile: false,
        },
        I::Call {
            result: None,
            function: Value::Function("mutate".into()),
            args: vec![],
            result_type: IrType::Void,
            stack_args_from: None,
        },
        I::InlineAsm {
            assembly: "STORE %0, R0, %1".into(),
            outputs: vec![],
            inputs: vec![AsmOperandIR {
                constraint: "r".into(),
                value: Value::Temp(0),
                tied_to: None,
            }],
            clobbers: vec!["memory".into()],
        },
        I::InlineAsm {
            assembly: "STORE T0, GP, T1".into(),
            outputs: vec![],
            inputs: vec![],
            clobbers: vec![],
        },
    ];
    for barrier in barriers {
        let mut f = sample();
        f.blocks[1].instructions.insert(0, barrier);
        eliminate_redundancy(&mut f);
        assert_eq!(
            f.blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .filter(|i| matches!(i, I::Load { .. }))
                .count(),
            2
        );
        assert!(f
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(i, I::Return(Some(Value::Constant(99))))));
    }
    let mut f = sample();
    f.blocks[0].instructions[0] = load(1, "width", true);
    eliminate_redundancy(&mut f);
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, I::Load { .. }))
            .count(),
        2
    );
}
#[test]
fn explicit_operand_only_asm_and_distinct_global_store_preserve_readonly_loads() {
    let mut f = sample();
    f.blocks[1].instructions.insert(
        0,
        I::InlineAsm {
            assembly: "ADD %0, %1, R0".into(),
            outputs: vec![],
            inputs: vec![],
            clobbers: vec![],
        },
    );
    f.blocks[1].instructions.insert(
        0,
        I::Store {
            value: Value::Constant(7),
            ptr: Value::Global("other".into()),
            value_type: IrType::I16,
            volatile: false,
        },
    );
    eliminate_redundancy(&mut f);
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, I::Load { .. }))
            .count(),
        1
    );
}
#[test]
fn a_bypass_edge_cannot_prove_a_condition() {
    let mut f = sample();
    f.parameters.push((5, IrType::I16));
    f.blocks[0].id = 4;
    f.blocks.insert(
        0,
        block(
            0,
            vec![I::BranchCond {
                condition: Value::Temp(5),
                true_label: 4,
                false_label: 1,
            }],
        ),
    );
    eliminate_redundancy(&mut f);
    assert!(f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, I::Return(Some(Value::Constant(99))))));
}
#[test]
fn cse_requires_dominance_and_preserves_comparison_signedness() {
    let mut f = Function::new("join".into(), IrType::I16);
    f.parameters = vec![(0, IrType::I16), (1, IrType::I16)];
    f.blocks = vec![
        block(
            0,
            vec![I::BranchCond {
                condition: Value::Temp(0),
                true_label: 1,
                false_label: 2,
            }],
        ),
        block(
            1,
            vec![
                bin(2, B::Add, Value::Temp(1), Value::Constant(7)),
                I::Return(Some(Value::Temp(2))),
            ],
        ),
        block(
            2,
            vec![
                bin(3, B::Add, Value::Temp(1), Value::Constant(7)),
                I::Return(Some(Value::Temp(3))),
            ],
        ),
    ];
    common_expressions(&mut f);
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, I::Binary { op: B::Add, .. }))
            .count(),
        2
    );
    let mut f = sample();
    if let I::Binary { op, .. } = &mut f.blocks[1].instructions[1] {
        *op = B::Sge;
    }
    eliminate_redundancy(&mut f);
    assert!(f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, I::Return(Some(Value::Constant(99))))));
}
#[test]
fn unreachable_phi_inputs_are_removed_before_phi_lowering() {
    let mut f = Function::new("phi".into(), IrType::I16);
    f.blocks = vec![
        block(
            0,
            vec![I::BranchCond {
                condition: Value::Constant(1),
                true_label: 1,
                false_label: 2,
            }],
        ),
        block(1, vec![I::Branch(3)]),
        block(2, vec![I::Branch(3)]),
        block(
            3,
            vec![
                I::Phi {
                    result: 0,
                    incoming: vec![(Value::Constant(5), 1), (Value::Constant(8), 2)],
                    result_type: IrType::I16,
                },
                I::Return(Some(Value::Temp(0))),
            ],
        ),
    ];
    eliminate_redundancy(&mut f);
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, I::Phi { .. })));
    super::super::phi::destroy_phis(&mut f).unwrap();
}

fn short_circuit(and: bool, selected_outcome: bool) -> Function {
    let mut f = Function::new("short_circuit_bounds".into(), IrType::I16);
    f.parameters = vec![(0, IrType::I16), (1, IrType::I16)];
    let op = if and { B::Ult } else { B::Uge };
    f.blocks = vec![
        block(
            0,
            vec![
                bin(2, op, Value::Temp(0), Value::Constant(8)),
                I::BranchCond {
                    condition: Value::Temp(2),
                    true_label: if and { 1 } else { 2 },
                    false_label: if and { 2 } else { 1 },
                },
            ],
        ),
        block(
            1,
            vec![bin(3, op, Value::Temp(1), Value::Constant(9)), I::Branch(3)],
        ),
        block(2, vec![I::Branch(3)]),
        block(
            3,
            vec![
                I::Phi {
                    result: 4,
                    incoming: vec![(Value::Temp(3), 1), (Value::Constant(i64::from(!and)), 2)],
                    result_type: IrType::I16,
                },
                I::BranchCond {
                    condition: Value::Temp(4),
                    true_label: if selected_outcome { 4 } else { 8 },
                    false_label: if selected_outcome { 8 } else { 4 },
                },
            ],
        ),
        block(
            4,
            vec![
                bin(5, B::Uge, Value::Temp(0), Value::Constant(8)),
                I::BranchCond {
                    condition: Value::Temp(5),
                    true_label: 5,
                    false_label: 6,
                },
            ],
        ),
        block(5, vec![I::Return(Some(Value::Constant(91)))]),
        block(
            6,
            vec![
                bin(6, B::Uge, Value::Temp(1), Value::Constant(9)),
                I::BranchCond {
                    condition: Value::Temp(6),
                    true_label: 7,
                    false_label: 8,
                },
            ],
        ),
        block(7, vec![I::Return(Some(Value::Constant(92)))]),
        block(8, vec![I::Return(Some(Value::Constant(0)))]),
    ];
    f
}

fn returns(f: &Function, value: i64) -> bool {
    f.blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, I::Return(Some(Value::Constant(c))) if *c == value))
}

#[test]
fn true_and_and_false_or_prove_both_complementary_bounds() {
    for and in [false, true] {
        let mut f = short_circuit(and, and);
        eliminate_redundancy(&mut f);
        assert!(
            !returns(&f, 91),
            "guard bound must be implied for && true / || false"
        );
        assert!(
            !returns(&f, 92),
            "RHS bound must be implied for && true / || false"
        );
    }
}

#[test]
fn false_and_and_true_or_do_not_select_a_unique_phi_edge() {
    for and in [false, true] {
        let mut f = short_circuit(and, !and);
        eliminate_redundancy(&mut f);
        assert!(returns(&f, 91));
        assert!(returns(&f, 92));
    }
}

#[test]
fn phi_facts_require_a_unique_control_edge_and_preserve_signedness() {
    let mut f = short_circuit(true, true);
    f.parameters.push((7, IrType::I16));
    f.blocks[0].id = 9;
    f.blocks.insert(
        0,
        block(
            0,
            vec![I::BranchCond {
                condition: Value::Temp(7),
                true_label: 9,
                false_label: 4,
            }],
        ),
    );
    eliminate_redundancy(&mut f);
    assert!(returns(&f, 91));
    assert!(returns(&f, 92));

    let mut f = short_circuit(true, true);
    if let I::Binary { op, .. } = &mut f.blocks[6].instructions[0] {
        *op = B::Sge;
    }
    eliminate_redundancy(&mut f);
    assert!(!returns(&f, 91));
    assert!(
        returns(&f, 92),
        "an unsigned bound cannot prove the signed comparison"
    );
}

#[test]
fn phi_facts_do_not_reuse_distinct_volatile_reads() {
    let mut f = short_circuit(true, true);
    f.blocks[0].instructions.insert(0, load(7, "x", true));
    if let I::Binary { lhs, .. } = &mut f.blocks[0].instructions[1] {
        *lhs = Value::Temp(7);
    }
    f.blocks[4].instructions.insert(0, load(8, "x", true));
    if let I::Binary { lhs, .. } = &mut f.blocks[4].instructions[1] {
        *lhs = Value::Temp(8);
    }
    eliminate_redundancy(&mut f);
    assert!(returns(&f, 91));
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, I::Load { volatile: true, .. }))
            .count(),
        2
    );
}

fn infer_phi(f: &Function, id: u32, label: u32) -> Option<HashMap<String, bool>> {
    let cfg = super::super::cfg::Cfg::new(f).unwrap();
    let defs = f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| super::super::result(i).map(|id| (id, i.clone())))
        .collect();
    let (constants, _) = super::super::constants::integer_facts(f);
    let block = f.blocks.iter().position(|b| b.id == label).unwrap();
    let context = PhiContext::new(f, &defs);
    short_circuit_facts(
        id,
        true,
        block,
        &context,
        &cfg,
        &defs,
        &constants,
        &vec![HashMap::new(); f.blocks.len()],
    )
}

#[test]
fn malformed_phi_inputs_and_duplicate_ssa_definitions_are_not_proofs() {
    for incoming in [
        vec![(Value::Temp(3), 1), (Value::Constant(0), 1)],
        vec![(Value::Temp(3), 1)],
        vec![(Value::Temp(3), 1), (Value::Constant(0), 99)],
        vec![
            (Value::Temp(3), 1),
            (Value::Constant(0), 2),
            (Value::Constant(0), 2),
        ],
    ] {
        let mut f = short_circuit(true, true);
        if let I::Phi {
            incoming: values, ..
        } = &mut f.blocks[3].instructions[0]
        {
            *values = incoming;
        }
        assert!(infer_phi(&f, 4, 3).is_none());
    }
    let mut f = short_circuit(true, true);
    f.blocks[2]
        .instructions
        .insert(0, bin(3, B::Ult, Value::Temp(1), Value::Constant(9)));
    assert!(infer_phi(&f, 4, 3).is_none());
}

#[test]
fn loop_carried_phi_and_rhs_only_operands_are_not_proofs() {
    let mut f = Function::new("loop_carried_condition".into(), IrType::I16);
    f.parameters.push((0, IrType::I16));
    f.blocks = vec![
        block(0, vec![I::Branch(1)]),
        block(
            1,
            vec![
                I::Phi {
                    result: 1,
                    incoming: vec![(Value::Constant(0), 0), (Value::Temp(2), 2)],
                    result_type: IrType::I16,
                },
                I::BranchCond {
                    condition: Value::Temp(1),
                    true_label: 2,
                    false_label: 3,
                },
            ],
        ),
        block(
            2,
            vec![
                bin(2, B::Ult, Value::Temp(0), Value::Constant(8)),
                I::Branch(1),
            ],
        ),
        block(3, vec![I::Return(Some(Value::Constant(0)))]),
    ];
    assert!(infer_phi(&f, 1, 1).is_none());
    f = short_circuit(true, true);
    f.blocks[1]
        .instructions
        .insert(0, bin(7, B::Add, Value::Temp(1), Value::Constant(2)));
    if let I::Binary { lhs, .. } = &mut f.blocks[1].instructions[1] {
        *lhs = Value::Temp(7);
    }
    assert!(
        infer_phi(&f, 4, 3).is_none(),
        "RHS-only expressions do not dominate the join"
    );
}

#[test]
fn repeated_rhs_arithmetic_gains_real_dominance_before_phi_fact_propagation() {
    for and in [false, true] {
        let mut f = short_circuit(and, and);
        f.blocks[1]
            .instructions
            .insert(0, bin(7, B::Add, Value::Temp(1), Value::Constant(2)));
        if let I::Binary { lhs, .. } = &mut f.blocks[1].instructions[1] {
            *lhs = Value::Temp(7);
        }
        f.blocks[6]
            .instructions
            .insert(0, bin(8, B::Add, Value::Temp(1), Value::Constant(2)));
        if let I::Binary { lhs, .. } = &mut f.blocks[6].instructions[1] {
            *lhs = Value::Temp(8);
        }
        assert!(infer_phi(&f, 4, 3).is_none());
        eliminate_redundancy(&mut f);
        assert!(!returns(&f, 91));
        assert!(!returns(&f, 92));
        assert_eq!(
            f.blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .filter(|inst| matches!(
                    inst,
                    I::Binary {
                        op: B::Add,
                        lhs: Value::Temp(1),
                        rhs: Value::Constant(2),
                        ..
                    }
                ))
                .count(),
            1
        );
        assert!(f.blocks[0].instructions.iter().any(|inst| matches!(
            inst,
            I::Binary {
                op: B::Add,
                lhs: Value::Temp(1),
                rhs: Value::Constant(2),
                ..
            }
        )));
    }
}

fn arithmetic_diamond(op: B) -> Function {
    let mut f = Function::new("arithmetic_diamond".into(), IrType::I16);
    f.parameters = vec![(0, IrType::I16), (1, IrType::I16), (2, IrType::I16)];
    f.blocks = vec![
        block(
            0,
            vec![I::BranchCond {
                condition: Value::Temp(0),
                true_label: 1,
                false_label: 2,
            }],
        ),
        block(
            1,
            vec![
                bin(3, op, Value::Temp(1), Value::Temp(2)),
                I::Return(Some(Value::Temp(3))),
            ],
        ),
        block(
            2,
            vec![
                bin(4, op, Value::Temp(1), Value::Temp(2)),
                I::Return(Some(Value::Temp(4))),
            ],
        ),
    ];
    f
}

#[test]
fn common_arithmetic_hoists_only_total_one_word_operations() {
    for op in [B::Add, B::Sub, B::And, B::Or, B::Xor] {
        let mut f = arithmetic_diamond(op);
        hoist_common_arithmetic(&mut f);
        assert!(matches!(
            f.blocks[0].instructions.first(),
            Some(I::Binary { result: 3, .. })
        ));
        common_expressions(&mut f);
        assert!(matches!(
            f.blocks[2].instructions.first(),
            Some(I::Binary {
                op: B::Add,
                lhs: Value::Temp(3),
                rhs: Value::Constant(0),
                ..
            })
        ));
    }
    for op in [
        B::SDiv,
        B::UDiv,
        B::SRem,
        B::URem,
        B::Shl,
        B::LShr,
        B::AShr,
        B::Mul,
    ] {
        let mut f = arithmetic_diamond(op);
        let before = format!("{f:?}");
        hoist_common_arithmetic(&mut f);
        assert_eq!(format!("{f:?}"), before);
    }
    let mut f = arithmetic_diamond(B::Add);
    for block in &mut f.blocks {
        for inst in &mut block.instructions {
            if let I::Binary { result_type, .. } = inst {
                *result_type = IrType::I32;
            }
        }
    }
    let before = format!("{f:?}");
    hoist_common_arithmetic(&mut f);
    assert_eq!(format!("{f:?}"), before);
}

#[test]
fn common_arithmetic_keeps_phi_induction_values_inside_their_iteration() {
    let mut f = arithmetic_diamond(B::Add);
    f.blocks[0].id = 1;
    f.blocks[1].id = 2;
    f.blocks[2].id = 3;
    f.blocks[0].instructions = vec![
        I::Phi {
            result: 5,
            incoming: vec![(Value::Constant(0), 0), (Value::Temp(6), 4)],
            result_type: IrType::I16,
        },
        I::BranchCond {
            condition: Value::Temp(0),
            true_label: 2,
            false_label: 3,
        },
    ];
    for block in &mut f.blocks[1..] {
        if let I::Binary { lhs, .. } = &mut block.instructions[0] {
            *lhs = Value::Temp(5);
        }
        *block.instructions.last_mut().unwrap() = I::Branch(4);
    }
    f.blocks.insert(0, block(0, vec![I::Branch(1)]));
    f.blocks.push(block(
        4,
        vec![
            bin(6, B::Add, Value::Temp(5), Value::Constant(1)),
            I::Branch(1),
        ],
    ));
    hoist_common_arithmetic(&mut f);
    assert!(matches!(
        f.blocks[0].instructions.as_slice(),
        [I::Branch(1)]
    ));
    assert!(matches!(
        f.blocks[1].instructions[1],
        I::Binary {
            result: 3,
            lhs: Value::Temp(5),
            ..
        }
    ));
}

#[test]
fn common_arithmetic_does_not_merge_volatile_operands_or_cross_opaque_asm() {
    let mut f = arithmetic_diamond(B::Add);
    for (bi, id) in [(1, 5), (2, 6)] {
        f.blocks[bi]
            .instructions
            .insert(0, load(id, "changing", true));
        if let I::Binary { lhs, .. } = &mut f.blocks[bi].instructions[1] {
            *lhs = Value::Temp(id);
        }
    }
    let before = format!("{f:?}");
    hoist_common_arithmetic(&mut f);
    assert_eq!(format!("{f:?}"), before);
    f = arithmetic_diamond(B::Add);
    f.blocks[1].instructions.insert(
        0,
        I::InlineAsm {
            assembly: "CALL unknown".into(),
            inputs: vec![],
            outputs: vec![],
            clobbers: vec![],
        },
    );
    let before = format!("{f:?}");
    hoist_common_arithmetic(&mut f);
    assert_eq!(format!("{f:?}"), before);
}
