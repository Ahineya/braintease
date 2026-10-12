use super::*;
use crate::{
    globals::GlobalManager,
    instr::tests::machine::Machine,
    lower::function::{calculate_local_slots, lower_function_v2},
    naming::NameGenerator,
    regmgmt::RegisterPressureManager,
};
use rcc_codegen::{AsmInst, Reg};
use rcc_frontend::ir::{FatPointer, IrBinaryOp as B};
use rcc_frontend::BankTag;

fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
    let mut b = BasicBlock::new(id);
    b.instructions = instructions;
    b
}
fn binary(result: u32, op: B, lhs: Value, rhs: Value, result_type: IrType) -> Instruction {
    Instruction::Binary {
        result,
        op,
        lhs,
        rhs,
        result_type,
    }
}
fn execute(function: &Function, args: &[u16]) -> Machine {
    let mut mgr = RegisterPressureManager::new(calculate_local_slots(function));
    mgr.init();
    let mut asm = lower_function_v2(
        function,
        &mut mgr,
        &mut NameGenerator::new(0),
        &GlobalManager::new(),
        64000,
    )
    .unwrap();
    assert!(matches!(asm.pop(), Some(AsmInst::Jalr(..))));
    let mut m = Machine::new(64000);
    m.regs[Reg::Sp as usize] = 1000;
    m.regs[Reg::Fp as usize] = 700;
    for (&reg, &arg) in [Reg::A0, Reg::A1, Reg::A2, Reg::A3].iter().zip(args) {
        m.regs[reg as usize] = arg;
    }
    m.run(&asm);
    assert_eq!(m.regs[Reg::Sp as usize], 1000);
    assert_eq!(m.regs[Reg::Fp as usize], 700);
    m
}

#[test]
fn cyclic_parallel_phis_and_critical_edges_preserve_all_integer_words() {
    for ty in [IrType::I16, IrType::I32, IrType::I64] {
        let mut f = Function::new("phi_swap".into(), ty.clone());
        f.add_parameter(0, IrType::I16);
        let x = 0x123456789abcdef0u64;
        let y = 0xfedcba9876543210u64;
        f.blocks = vec![
            block(
                0,
                vec![Instruction::BranchCond {
                    condition: Value::Temp(0),
                    true_label: 1,
                    false_label: 3,
                }],
            ),
            block(
                1,
                vec![
                    Instruction::Phi {
                        result: 1,
                        incoming: vec![(Value::Constant(x as i64), 0), (Value::Temp(2), 2)],
                        result_type: ty.clone(),
                    },
                    Instruction::Phi {
                        result: 2,
                        incoming: vec![(Value::Constant(y as i64), 0), (Value::Temp(1), 2)],
                        result_type: ty.clone(),
                    },
                    Instruction::Phi {
                        result: 3,
                        incoming: vec![(Value::Temp(0), 0), (Value::Temp(4), 2)],
                        result_type: IrType::I16,
                    },
                    Instruction::BranchCond {
                        condition: Value::Temp(3),
                        true_label: 2,
                        false_label: 4,
                    },
                ],
            ),
            block(
                2,
                vec![
                    binary(4, B::Sub, Value::Temp(3), Value::Constant(1), IrType::I16),
                    Instruction::Branch(1),
                ],
            ),
            block(3, vec![Instruction::Return(Some(Value::Constant(77)))]),
            block(4, vec![Instruction::Return(Some(Value::Temp(1)))]),
        ];
        destroy_phis(&mut f).unwrap();
        assert_eq!(f.blocks.len(), 6, "critical entry edge must be split");
        assert!(!f
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(i, Instruction::Phi { .. })));
        for count in [0, 1, 2, 3, 18, 19] {
            let expected = if count == 0 {
                77
            } else if count % 2 == 0 {
                x
            } else {
                y
            };
            let m = execute(&f, &[count]);
            for (word, &reg) in [Reg::Rv0, Reg::Rv1, Reg::X0, Reg::X1]
                .iter()
                .take(ty.abi_slots())
                .enumerate()
            {
                assert_eq!(
                    m.regs[reg as usize],
                    (expected >> (word * 16)) as u16,
                    "{ty:?}, count {count}, word {word}"
                );
            }
        }
    }
}

#[test]
fn pointer_and_float_phi_homes_preserve_typed_bits() {
    for ty in [
        IrType::FatPtr(Box::new(IrType::I16)),
        IrType::F32,
        IrType::F64,
    ] {
        let mut f = Function::new("typed_phi".into(), ty.clone());
        f.add_parameter(0, IrType::I16);
        let values = if ty.is_pointer() {
            vec![
                Value::FatPtr(FatPointer {
                    addr: Box::new(Value::Constant(63999)),
                    bank: BankTag::Heap(41),
                }),
                Value::FatPtr(FatPointer {
                    addr: Box::new(Value::Constant(0)),
                    bank: BankTag::Null,
                }),
            ]
        } else {
            vec![
                Value::Constant(0x7ff812348000ffffu64 as i64),
                Value::Constant(0x8000000080000000u64 as i64),
            ]
        };
        f.blocks = vec![
            block(
                0,
                vec![Instruction::BranchCond {
                    condition: Value::Temp(0),
                    true_label: 1,
                    false_label: 2,
                }],
            ),
            block(1, vec![Instruction::Branch(3)]),
            block(2, vec![Instruction::Branch(3)]),
            block(
                3,
                vec![
                    Instruction::Phi {
                        result: 1,
                        incoming: vec![(values[0].clone(), 1), (values[1].clone(), 2)],
                        result_type: ty.clone(),
                    },
                    Instruction::Return(Some(Value::Temp(1))),
                ],
            ),
        ];
        destroy_phis(&mut f).unwrap();
        for condition in [0, 1] {
            // Stored NULL pointers use the ABI's tagged representation.
            let expected = if ty.is_pointer() {
                if condition == 1 {
                    (41u64 << 16) | 63999
                } else {
                    (crate::regmgmt::BankTagValue::NULL as u16 as u64) << 16
                }
            } else if condition == 1 {
                0x7ff812348000ffff
            } else {
                0x8000000080000000
            };
            let m = execute(&f, &[condition]);
            for (word, &reg) in [Reg::Rv0, Reg::Rv1, Reg::X0, Reg::X1]
                .iter()
                .take(ty.abi_slots())
                .enumerate()
            {
                assert_eq!(
                    m.regs[reg as usize],
                    (expected >> (word * 16)) as u16,
                    "{ty:?}, condition {condition}, word {word}"
                );
            }
        }
    }
}

#[test]
fn malformed_phi_fails_without_mutating_the_function() {
    let mut base = Function::new("bad_phi".into(), IrType::I16);
    base.blocks = vec![
        block(0, vec![Instruction::Branch(1)]),
        block(
            1,
            vec![
                Instruction::Phi {
                    result: 2,
                    incoming: vec![(Value::Constant(1), 0)],
                    result_type: IrType::I16,
                },
                Instruction::Return(Some(Value::Temp(2))),
            ],
        ),
    ];
    for case in 0..4 {
        let mut f = base.clone();
        if let Instruction::Phi {
            incoming,
            result_type,
            ..
        } = &mut f.blocks[1].instructions[0]
        {
            match case {
                0 => incoming.clear(),
                1 => incoming.push((Value::Constant(2), 0)),
                2 => incoming[0].1 = 99,
                _ => *result_type = IrType::Void,
            }
        }
        let before = f.clone();
        assert!(destroy_phis(&mut f).is_err());
        assert_eq!(f, before);
    }
}

#[test]
fn implicit_fallthrough_edges_are_materialized_before_splitting() {
    let mut f = Function::new("fallthrough_phi".into(), IrType::I16);
    f.blocks = vec![
        block(0, vec![]),
        block(
            1,
            vec![
                Instruction::Phi {
                    result: 1,
                    incoming: vec![(Value::Constant(123), 0)],
                    result_type: IrType::I16,
                },
                Instruction::Return(Some(Value::Temp(1))),
            ],
        ),
    ];
    destroy_phis(&mut f).unwrap();
    assert_eq!(execute(&f, &[]).regs[Reg::Rv0 as usize], 123);
}
