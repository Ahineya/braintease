use super::*;
use rcc_frontend::ir::{AsmOperandIR, BasicBlock};
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
    eliminate_redundancy(&mut f);
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
