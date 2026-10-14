use super::*;
use crate::{
    globals::GlobalManager,
    instr::tests::machine::Machine,
    lower::function::{calculate_local_slots, lower_function_v2},
    naming::NameGenerator,
    regmgmt::RegisterPressureManager,
};
use rcc_codegen::{AsmInst, Reg};
use rcc_ir::{ir::FatPointer, BankTag};

fn address() -> Value {
    Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(100)),
        bank: BankTag::Heap(0),
    })
}

fn counted(signed: bool) -> Function {
    let mut f = Function::new("counted".into(), IrType::I16);
    f.parameters = vec![(0, IrType::I16), (1, IrType::I16)];
    f.blocks = [
        vec![I::Branch(1)],
        vec![
            I::Phi {
                result: 2,
                incoming: vec![(Value::Temp(0), 0), (Value::Temp(4), 3)],
                result_type: IrType::I16,
            },
            I::Binary {
                result: 3,
                op: if signed { B::Slt } else { B::Ult },
                lhs: Value::Temp(2),
                rhs: Value::Temp(1),
                result_type: IrType::I16,
            },
            I::BranchCond {
                condition: Value::Temp(3),
                true_label: 2,
                false_label: 4,
            },
        ],
        vec![
            I::Store {
                value: Value::Temp(2),
                ptr: address(),
                value_type: IrType::I16,
                volatile: true,
            },
            I::Branch(3),
        ],
        vec![
            I::Binary {
                result: 4,
                op: B::Add,
                lhs: Value::Temp(2),
                rhs: Value::Constant(1),
                result_type: IrType::I16,
            },
            I::Branch(1),
        ],
        vec![
            I::Load {
                result: 5,
                ptr: address(),
                result_type: IrType::I16,
                volatile: true,
            },
            I::Return(Some(Value::Temp(5))),
        ],
    ]
    .into_iter()
    .enumerate()
    .map(|(id, instructions)| {
        let mut b = BasicBlock::new(id as u32);
        b.instructions = instructions;
        b
    })
    .collect();
    f
}

fn lower(mut function: Function) -> Vec<AsmInst> {
    super::super::destroy_phis(&mut function).unwrap();
    let mut manager = RegisterPressureManager::new(calculate_local_slots(&function));
    manager.init();
    let mut code = lower_function_v2(
        &function,
        &mut manager,
        &mut NameGenerator::new(0),
        &GlobalManager::new(),
        64000,
    )
    .unwrap();
    assert!(matches!(code.pop(), Some(AsmInst::Jalr(..))));
    code
}

fn execute(code: &[AsmInst], initial: u16, bound: u16) -> Machine {
    let mut machine = Machine::new(64000);
    machine.regs[Reg::Sp as usize] = 1000;
    machine.regs[Reg::Fp as usize] = 700;
    machine.regs[Reg::A0 as usize] = initial;
    machine.regs[Reg::A1 as usize] = bound;
    machine.memory.insert((0, 100), 12345);
    for (i, reg) in [Reg::S0, Reg::S1, Reg::S2, Reg::S3].iter().enumerate() {
        machine.regs[*reg as usize] = 800 + i as u16;
    }
    machine.run(code);
    assert_eq!(machine.regs[Reg::Sp as usize], 1000);
    assert_eq!(machine.regs[Reg::Fp as usize], 700);
    for (i, reg) in [Reg::S0, Reg::S1, Reg::S2, Reg::S3].iter().enumerate() {
        assert_eq!(machine.regs[*reg as usize], 800 + i as u16);
    }
    machine
}

#[test]
fn rotation_retains_entry_guard_and_private_counter_initialization() {
    for signed in [false, true] {
        let mut f = counted(signed);
        rotate(&mut f);
        assert_eq!(f.blocks.len(), 7);
        assert!(matches!(f.blocks[0].instructions[1], I::Binary { op: B::Slt, .. }) == signed);
        assert!(matches!(
            f.blocks[0].instructions[1],
            I::Binary {
                lhs: Value::Temp(0),
                rhs: Value::Temp(1),
                ..
            }
        ));
        let head = f.blocks.iter().find(|b| b.id == 1).unwrap();
        assert!(matches!(
            head.instructions[0],
            I::Load {
                result: 2,
                volatile: false,
                ..
            }
        ));
        assert_eq!(head.predecessors.len(), 2);
        let tail = f.blocks.iter().find(|b| b.id == 3).unwrap();
        assert!(matches!(
            tail.instructions[2],
            I::Binary {
                op: B::Eq,
                lhs: Value::Temp(4),
                rhs: Value::Temp(1),
                ..
            }
        ));
        let once = f.clone();
        rotate(&mut f);
        assert_eq!(f, once, "rotation is idempotent");
    }
}

#[test]
fn emitted_code_preserves_dynamic_zero_trips_and_native_word_endpoints() {
    for signed in [false, true] {
        let before = lower(counted(signed));
        let mut f = counted(signed);
        rotate(&mut f);
        let after = lower(f);
        for (initial, bound) in [
            (0, 0),
            (7, 3),
            (0, 64),
            (32765, 32767),
            (32768, 32770),
            (65533, 65535),
            (65535, 0),
            (32767, 32768),
        ] {
            let old = execute(&before, initial, bound);
            let new = execute(&after, initial, bound);
            let enters = if signed {
                (initial as i16) < (bound as i16)
            } else {
                initial < bound
            };
            assert_eq!(
                new.regs[Reg::Rv0 as usize],
                if enters { bound.wrapping_sub(1) } else { 12345 },
                "signed={signed} initial={initial} bound={bound}"
            );
            assert_eq!(new.memory[&(0, 100)], old.memory[&(0, 100)]);
            if initial == 0 && bound == 64 {
                assert!(
                    new.executed_instructions < old.executed_instructions,
                    "rotation must improve emitted loop: {} vs {}\n{after:#?}",
                    new.executed_instructions,
                    old.executed_instructions
                );
            }
        }
    }
}

#[test]
fn module_pipeline_keeps_same_bank_address_proofs_before_rotating() {
    let mut f = counted(false);
    if let I::Binary { rhs, .. } = &mut f.blocks[1].instructions[1] {
        *rhs = Value::Constant(64);
    }
    f.blocks[2].instructions.insert(
        0,
        I::GetElementPtr {
            result: 6,
            ptr: address(),
            indices: vec![Value::Temp(2)],
            result_type: IrType::FatPtr(Box::new(IrType::I16)),
            source_element_type: IrType::I16,
            is_struct_field: false,
        },
    );
    if let I::Store { ptr, .. } = &mut f.blocks[2].instructions[1] {
        *ptr = Value::Temp(6);
    }
    let mut module = rcc_ir::ir::Module::new("counted_pipeline".into());
    module.functions.push(f);
    let mut code = crate::lower::module::lower_module(&module, 64000, false).unwrap();
    assert!(matches!(code.pop(), Some(AsmInst::Jalr(..))));
    assert!(
        !code.iter().any(|i| matches!(
            i,
            AsmInst::Div(..) | AsmInst::Mod(..) | AsmInst::DivI(..) | AsmInst::ModI(..)
        )),
        "a proved same-bank GEP must not regain bank carry arithmetic"
    );
    for initial in [0, 63, 64, 65] {
        let machine = execute(&code, initial, 0);
        for index in initial..64 {
            assert_eq!(machine.memory.get(&(0, 100 + index)), Some(&index));
        }
        if initial == 0 {
            assert!(
                machine.executed_instructions < 400,
                "hot loop should have four instructions: {}",
                machine.executed_instructions
            );
        }
    }
}

#[test]
fn operand_only_assembly_keeps_memory_effects_and_can_rotate() {
    use rcc_ir::ir::AsmOperandIR;
    let mut f = counted(false);
    f.blocks[2].instructions[0] = I::InlineAsm {
        assembly: "STORE %0, R0, %1".into(),
        outputs: vec![],
        inputs: vec![
            AsmOperandIR {
                constraint: "r".into(),
                value: Value::Temp(2),
                tied_to: None,
            },
            AsmOperandIR {
                constraint: "r".into(),
                value: Value::Constant(100),
                tied_to: None,
            },
        ],
        clobbers: vec!["memory".into()],
    };
    let before = lower(f.clone());
    rotate(&mut f);
    assert_eq!(f.blocks.len(), 7);
    let after = lower(f);
    for (start, end) in [(0, 64), (17, 19), (19, 17)] {
        let old = execute(&before, start, end);
        let new = execute(&after, start, end);
        assert_eq!(new.memory[&(0, 100)], old.memory[&(0, 100)]);
        if start == 0 {
            assert!(new.executed_instructions < old.executed_instructions);
        }
    }
    let mut f = counted(false);
    f.blocks[2].instructions[0] = I::InlineAsm {
        assembly: "STORE %0, SB, FP".into(),
        outputs: vec![],
        inputs: vec![AsmOperandIR {
            constraint: "r".into(),
            value: Value::Temp(2),
            tied_to: None,
        }],
        clobbers: vec!["memory".into()],
    };
    let old = f.clone();
    rotate(&mut f);
    assert_eq!(f, old, "frame-dependent asm is still rejected");
}

#[test]
fn nonunit_or_wrapping_counters_and_noninvariant_bounds_are_rejected() {
    let mut cases = vec![];
    for comparison in [B::Ule, B::Sle, B::Ne] {
        let mut f = counted(false);
        if let I::Binary { op, .. } = &mut f.blocks[1].instructions[1] {
            *op = comparison;
        }
        cases.push(f);
    }
    let mut f = counted(false);
    if let I::Binary { rhs, .. } = &mut f.blocks[3].instructions[0] {
        *rhs = Value::Constant(2);
    }
    cases.push(f);
    let mut f = counted(false);
    f.blocks[1].instructions.insert(
        1,
        I::Load {
            result: 6,
            ptr: address(),
            result_type: IrType::I16,
            volatile: true,
        },
    );
    if let I::Binary { rhs, .. } = &mut f.blocks[1].instructions[2] {
        *rhs = Value::Temp(6);
    }
    cases.push(f);
    let mut f = counted(false);
    f.parameters[1].1 = IrType::I32;
    cases.push(f);
    for mut f in cases {
        let old = f.clone();
        rotate(&mut f);
        assert_eq!(f, old);
    }
}

#[test]
fn liveouts_multiple_entries_and_skipped_increments_are_rejected() {
    let mut cases = vec![];
    let mut f = counted(false);
    f.blocks[4].instructions = vec![I::Return(Some(Value::Temp(2)))];
    cases.push(f);
    let mut f = counted(false);
    f.blocks[2].instructions.insert(
        0,
        I::Store {
            value: Value::Temp(3),
            ptr: address(),
            value_type: IrType::I16,
            volatile: true,
        },
    );
    cases.push(f);
    let mut f = counted(false);
    f.blocks[2].instructions[1] = I::BranchCond {
        condition: Value::Temp(0),
        true_label: 1,
        false_label: 3,
    };
    cases.push(f);
    let mut f = counted(false);
    let mut dead = BasicBlock::new(9);
    dead.instructions = vec![I::Branch(1)];
    f.blocks.push(dead);
    cases.push(f);
    let mut f = counted(false);
    f.blocks[0].instructions = vec![I::BranchCond {
        condition: Value::Temp(0),
        true_label: 1,
        false_label: 4,
    }];
    cases.push(f);
    for mut f in cases {
        let old = f.clone();
        rotate(&mut f);
        assert_eq!(f, old);
    }
}

#[test]
fn malformed_phis_and_unprofitable_calls_are_rejected() {
    let mut cases = vec![];
    for label in [0, 9] {
        let mut f = counted(false);
        if let I::Phi { incoming, .. } = &mut f.blocks[1].instructions[0] {
            incoming[1].1 = label;
        }
        cases.push(f);
    }
    let mut f = counted(false);
    f.blocks[2].instructions.insert(
        0,
        I::Binary {
            result: 4,
            op: B::Add,
            lhs: Value::Temp(2),
            rhs: Value::Constant(1),
            result_type: IrType::I16,
        },
    );
    cases.push(f);
    let mut f = counted(false);
    f.blocks[2].instructions.insert(
        0,
        I::Call {
            result: None,
            function: Value::Function("opaque".into()),
            args: vec![],
            result_type: IrType::Void,
            stack_args_from: None,
        },
    );
    cases.push(f);
    let mut f = counted(false);
    f.blocks[1].instructions.insert(
        0,
        I::Phi {
            result: 6,
            incoming: vec![(Value::Constant(0), 0), (Value::Temp(6), 3)],
            result_type: IrType::I16,
        },
    );
    cases.push(f);
    for mut f in cases {
        let old = f.clone();
        rotate(&mut f);
        assert_eq!(f, old);
    }
}
