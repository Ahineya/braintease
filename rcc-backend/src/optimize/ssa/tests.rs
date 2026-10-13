use super::*;
use rcc_frontend::ir::BasicBlock;

fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
    let mut b = BasicBlock::new(id);
    b.instructions = instructions;
    b
}
fn store(value: Value) -> Instruction {
    Instruction::Store {
        value,
        ptr: Value::Temp(10),
        value_type: IrType::I32,
        volatile: false,
    }
}
fn load(result: u32) -> Instruction {
    Instruction::Load {
        result,
        ptr: Value::Temp(10),
        result_type: IrType::I32,
        volatile: false,
    }
}
fn example() -> Function {
    let mut f = Function::new("ssa_loop".into(), IrType::I32);
    f.add_parameter(0, IrType::I16);
    f.add_parameter(1, IrType::I32);
    f.blocks = vec![
        block(
            0,
            vec![
                Instruction::Alloca {
                    result: 10,
                    alloc_type: IrType::I32,
                    count: None,
                    result_type: IrType::FatPtr(Box::new(IrType::I32)),
                },
                store(Value::Temp(1)),
                Instruction::Branch(1),
            ],
        ),
        block(
            1,
            vec![
                load(11),
                Instruction::BranchCond {
                    condition: Value::Temp(0),
                    true_label: 2,
                    false_label: 3,
                },
            ],
        ),
        block(
            2,
            vec![
                Instruction::Binary {
                    result: 12,
                    op: IrBinaryOp::Add,
                    lhs: Value::Temp(11),
                    rhs: Value::Constant(65537),
                    result_type: IrType::I32,
                },
                store(Value::Temp(12)),
                Instruction::Branch(1),
            ],
        ),
        block(
            3,
            vec![load(13), Instruction::Return(Some(Value::Temp(13)))],
        ),
    ];
    f
}

#[test]
fn promotes_loop_carried_locals_and_licm_respects_phi_dependencies() {
    let mut f = example();
    f.blocks[2].instructions.insert(
        0,
        Instruction::Binary {
            result: 14,
            op: IrBinaryOp::Mul,
            lhs: Value::Temp(1),
            rhs: Value::Constant(7),
            result_type: IrType::I32,
        },
    );
    promote_locals(&mut f);
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(
            i,
            Instruction::Load { .. } | Instruction::Store { .. } | Instruction::Alloca { .. }
        )));
    assert_eq!(
        f.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i, Instruction::Phi { .. }))
            .count(),
        1
    );
    let Instruction::Phi {
        result,
        incoming,
        result_type,
    } = &f.blocks[1].instructions[0]
    else {
        panic!("missing header Phi")
    };
    assert_eq!(*result_type, IrType::I32);
    assert_eq!(incoming, &vec![(Value::Temp(1), 0), (Value::Temp(12), 2)]);
    assert!(
        matches!(&f.blocks[1].instructions[1], Instruction::Binary { lhs: Value::Temp(id), .. } if id == result)
    );
    super::super::loops::hoist_invariants(&mut f);
    assert!(f.blocks[0]
        .instructions
        .iter()
        .any(|i| matches!(i, Instruction::Binary { result: 14, .. })));
    assert!(f.blocks[2]
        .instructions
        .iter()
        .any(|i| matches!(i, Instruction::Binary { result: 12, .. })));
}

#[test]
fn prunes_dead_phi_and_keeps_multiple_stores_in_program_order() {
    let mut f = example();
    f.blocks[1].instructions.remove(0);
    f.blocks[2].instructions = vec![
        store(Value::Constant(5)),
        store(Value::Constant(9)),
        Instruction::Branch(1),
    ];
    f.blocks[3].instructions = vec![
        store(Value::Constant(71)),
        load(13),
        Instruction::Return(Some(Value::Temp(13))),
    ];
    promote_locals(&mut f);
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, Instruction::Phi { .. })));
    assert!(matches!(
        &f.blocks[3].instructions[0],
        Instruction::Binary {
            lhs: Value::Constant(71),
            ..
        }
    ));
}

#[test]
fn leaves_uninitialized_volatile_address_taken_and_unreachable_uses_in_memory() {
    for case in 0..4 {
        let mut f = example();
        match case {
            0 => {
                f.blocks[0].instructions.remove(1);
            }
            1 => {
                if let Instruction::Load { volatile, .. } = &mut f.blocks[1].instructions[0] {
                    *volatile = true;
                }
            }
            2 => {
                f.blocks[0].instructions.insert(
                    2,
                    Instruction::Call {
                        result: None,
                        function: Value::Function("escape".into()),
                        args: vec![Value::Temp(10)],
                        result_type: IrType::Void,
                        stack_args_from: None,
                    },
                );
            }
            _ => f.blocks.push(block(
                4,
                vec![load(19), Instruction::Return(Some(Value::Temp(19)))],
            )),
        }
        let before = f.clone();
        promote_locals(&mut f);
        assert_eq!(f, before, "case {case}");
    }
}

#[test]
fn local_declared_inside_loop_is_promoted_when_each_read_is_initialized() {
    let mut f = example();
    let alloca = f.blocks[0].instructions.remove(0);
    f.blocks[0].instructions.remove(0);
    f.blocks[1].instructions.remove(0);
    f.blocks[2].instructions = vec![
        alloca,
        store(Value::Temp(1)),
        load(12),
        store(Value::Constant(73)),
        load(14),
        Instruction::Branch(1),
    ];
    f.blocks[3].instructions = vec![Instruction::Return(Some(Value::Temp(1)))];
    promote_locals(&mut f);
    assert!(!f
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(
            i,
            Instruction::Alloca { .. }
                | Instruction::Phi { .. }
                | Instruction::Load { .. }
                | Instruction::Store { .. }
        )));
}

fn pointer_example() -> Function {
    use rcc_frontend::ir::FatPointer;
    let pointer = IrType::FatPtr(Box::new(IrType::I16));
    let mut f = example();
    f.return_type = pointer.clone();
    f.parameters[1].1 = pointer.clone();
    for inst in f.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
        match inst {
            Instruction::Alloca { alloc_type, result_type, .. } => {
                *alloc_type = pointer.clone();
                *result_type = IrType::FatPtr(Box::new(pointer.clone()));
            }
            Instruction::Store { value_type, .. } => *value_type = pointer.clone(),
            Instruction::Load { result_type, .. } => *result_type = pointer.clone(),
            Instruction::Binary { result: 12, .. } => {
                *inst = Instruction::GetElementPtr {
                    result: 12,
                    ptr: Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(11)), bank: BankTag::Mixed }),
                    indices: vec![Value::Constant(1)],
                    result_type: pointer.clone(), source_element_type: IrType::I16,
                    is_struct_field: false,
                };
            }
            _ => {}
        }
    }
    f
}

#[test]
fn mutable_pointer_loop_aliases_preserve_both_components_and_phi_edges() {
    let mut f = pointer_example();
    promote_locals(&mut f);
    let Instruction::Phi { result, incoming, result_type } = &f.blocks[1].instructions[0] else {
        panic!("missing pointer Phi");
    };
    assert!(result_type.is_pointer());
    assert_eq!(incoming, &vec![(Value::Temp(1), 0), (Value::Temp(12), 2)]);
    assert!(matches!(&f.blocks[2].instructions[0], Instruction::GetElementPtr { ptr: Value::FatPtr(p), .. }
        if *p.addr == Value::Temp(*result) && p.bank == BankTag::Mixed));
    assert_eq!(f.blocks[3].instructions[0], Instruction::Return(Some(Value::Temp(*result))));
    assert!(!f.blocks.iter().flat_map(|b| &b.instructions).any(|inst|
        matches!(inst, Instruction::Load { .. } | Instruction::Store { .. } | Instruction::Alloca { .. } | Instruction::Binary { .. })));
    // Phi destruction must copy a pointer pair, never turn it into an integer.
    super::super::phi::destroy_phis(&mut f).unwrap();
    for inst in f.blocks.iter().flat_map(|b| &b.instructions) {
        match inst {
            Instruction::Load { result_type, .. } => assert!(result_type.is_pointer()),
            Instruction::Store { value_type, .. } => assert!(value_type.is_pointer()),
            _ => {}
        }
    }
}

#[test]
fn mutable_pointer_promotion_keeps_unsafe_or_untyped_sources_in_memory() {
    use rcc_frontend::ir::FatPointer;
    for case in 0..7 {
        let mut f = pointer_example();
        match case {
            0 => if let Instruction::Load { volatile, .. } = &mut f.blocks[1].instructions[0] { *volatile = true; },
            1 => f.blocks[0].instructions.insert(2, Instruction::Call {
                result: None, function: Value::Function("escape".into()), args: vec![Value::Temp(10)],
                result_type: IrType::Void, stack_args_from: None,
            }),
            2 => { f.blocks[0].instructions.remove(1); },
            3 => if let Instruction::Store { value, .. } = &mut f.blocks[0].instructions[1] { *value = Value::Constant(0); },
            4 => if let Instruction::Store { value, .. } = &mut f.blocks[0].instructions[1] {
                *value = Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(1)), bank: BankTag::Heap(7) });
            },
            5 => f.parameters[1].1 = IrType::I16,
            _ => if let Instruction::Store { volatile, .. } = &mut f.blocks[2].instructions[1] { *volatile = true; },
        }
        let before = f.clone();
        promote_locals(&mut f);
        assert_eq!(f, before, "case {case}");
    }
}

#[test]
fn pointer_aliases_can_feed_a_second_promoted_loop_local() {
    let mut f = pointer_example();
    let pointer = f.parameters[1].1.clone();
    f.blocks[0].instructions.insert(1, Instruction::Alloca {
        result: 20, alloc_type: pointer.clone(), count: None,
        result_type: IrType::FatPtr(Box::new(pointer.clone())),
    });
    f.blocks[0].instructions.insert(3, Instruction::Store {
        value: Value::Temp(1), ptr: Value::Temp(20), value_type: pointer.clone(), volatile: false,
    });
    f.blocks[2].instructions.insert(0, Instruction::Store {
        value: Value::Temp(11), ptr: Value::Temp(20), value_type: pointer.clone(), volatile: false,
    });
    f.blocks[3].instructions = vec![Instruction::Load {
        result: 21, ptr: Value::Temp(20), result_type: pointer, volatile: false,
    }, Instruction::Return(Some(Value::Temp(21)))];
    promote_locals(&mut f);
    assert_eq!(f.blocks.iter().flat_map(|b| &b.instructions).filter(|inst| matches!(inst, Instruction::Phi { .. })).count(), 2);
    assert!(!f.blocks.iter().flat_map(|b| &b.instructions).any(|inst|
        matches!(inst, Instruction::Load { .. } | Instruction::Store { .. } | Instruction::Alloca { .. })));
}
