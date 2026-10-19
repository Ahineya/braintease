use super::*;
use crate::instr::tests::machine::Machine;
use crate::naming::new_function_naming;

#[test]
fn zero_offset_does_not_build_an_extra_address_and_bank_pair() {
    let mut manager = RegisterPressureManager::new(0);
    manager.init();
    let mut naming = new_function_naming();
    let name = naming.temp_name(0);
    let input = manager.get_register(name.clone());
    manager.take_instructions();
    manager.set_pointer_bank(name, BankInfo::Heap(3));
    let code = offset(
        &mut manager,
        &mut naming,
        &Value::Temp(0),
        &Value::Constant(0),
        1,
        1,
        8192,
    );
    let mut machine = Machine::new(8192);
    machine.regs[input as usize] = 123;
    machine.run(&code);
    assert!(
        machine.executed_instructions <= 8,
        "p[0] needs only component copies and one bounds check, got {} instructions",
        machine.executed_instructions
    );
}

#[test]
fn owned_pointer_results_preserve_live_inputs_and_carry_borrow() {
    for size in [512u16, 8192, 64000, 65535] {
        for address in [0, 1, size - 1] {
            for delta in [
                -(size as i64) * 2,
                -1,
                0,
                1,
                size as i64 - 1,
                size as i64,
                size as i64 * 2 + 1,
            ] {
                let mut manager = RegisterPressureManager::new(0);
                manager.init();
                let mut naming = new_function_naming();
                let name = naming.temp_name(0);
                let source = manager.get_register(name.clone());
                manager.take_instructions();
                manager.set_pointer_bank(name.clone(), BankInfo::Heap(9));
                let mut code = offset(
                    &mut manager,
                    &mut naming,
                    &Value::Temp(0),
                    &Value::Constant(delta),
                    1,
                    1,
                    size,
                );
                let (lo, hi) = components(&mut manager, &mut naming, &Value::Temp(1), &mut code);
                code.push(AsmInst::Move(Reg::Rv0, lo));
                code.push(AsmInst::Move(Reg::Rv1, hi));
                let original = manager.get_register(name);
                code.extend(manager.take_instructions());
                code.push(AsmInst::Move(Reg::A0, original));
                let mut machine = Machine::new(size);
                machine.regs[source as usize] = address;
                machine.run(&code);
                let linear = 9i64 * size as i64 + address as i64 + delta;
                assert_eq!(
                    machine.regs[Reg::Rv0 as usize],
                    (linear % size as i64) as u16
                );
                assert_eq!(
                    machine.regs[Reg::Rv1 as usize],
                    (linear / size as i64) as u16
                );
                assert_eq!(
                    machine.regs[Reg::A0 as usize],
                    address,
                    "source changed after {address}+{delta}, bank {size}"
                );
            }
        }
    }
}

#[test]
fn constant_steps_use_one_boundary_test_and_preserve_raw_offsets() {
    for size in [1u16, 2, 512, 8192, 64000, 65535] {
        for address in [0, 1, size - 1, size, 32768, 65535] {
            for delta in [-1i64, 0, 1, size as i64 - 1, -(size as i64) + 1] {
                let mut manager = RegisterPressureManager::new(0);
                manager.init();
                let mut naming = new_function_naming();
                let name = naming.temp_name(0);
                let source = manager.get_register(name.clone());
                manager.take_instructions();
                manager.set_pointer_bank(name, BankInfo::Heap(9));
                let mut code = offset(&mut manager, &mut naming, &Value::Temp(0),
                    &Value::Constant(delta), 1, 1, size);
                let mut machine = Machine::new(size);
                machine.regs[source as usize] = address;
                machine.run(&code);
                if size > 1 && delta.abs() <= 1 {
                    assert!(machine.executed_instructions <= 8,
                        "{address}+{delta}, bank size {size}: {} instructions",
                        machine.executed_instructions);
                }
                let (lo, hi) = components(&mut manager, &mut naming, &Value::Temp(1), &mut code);
                code.push(AsmInst::Move(Reg::Rv0, lo));
                code.push(AsmInst::Move(Reg::Rv1, hi));
                let mut machine = Machine::new(size);
                machine.regs[source as usize] = address;
                machine.run(&code);
                // Preserve the existing single-bank adjustment for raw integer
                // casts too, even when their offset exceeds a whole small bank.
                let r = delta.rem_euclid(size as i64) as u32;
                let sum = address as u32 + r;
                let adjust = sum >= size as u32;
                let expected_address = if adjust { sum.wrapping_sub(size as u32) } else { sum } as u16;
                let expected_bank = (9 + delta.div_euclid(size as i64) + i64::from(adjust)) as u16;
                assert_eq!(machine.regs[Reg::Rv0 as usize], expected_address,
                    "address {address}+{delta}, bank size {size}");
                assert_eq!(machine.regs[Reg::Rv1 as usize], expected_bank);
            }
        }
    }
}

#[test]
fn native_word_offsets_normalize_both_words_before_adding() {
    for size in [1u16, 512, 8192, 64000, 65535] {
        for address in [0, 1, size - 1, size, 65535] {
            for delta in [0, 1, size - 1, size, 65535] {
                let mut manager = RegisterPressureManager::new(0);
                manager.init();
                let mut naming = new_function_naming();
                let name = naming.temp_name(0);
                let source = manager.get_register(name.clone());
                manager.set_pointer_bank(name, BankInfo::Heap(9));
                let index = manager.get_register(naming.temp_name(2));
                manager.take_instructions();
                let mut code = offset(&mut manager, &mut naming, &Value::Temp(0),
                    &Value::Temp(2), 1, 1, size);
                let (lo, hi) = components(&mut manager, &mut naming, &Value::Temp(1), &mut code);
                code.push(AsmInst::Move(Reg::Rv0, lo));
                code.push(AsmInst::Move(Reg::Rv1, hi));
                let mut machine = Machine::new(size);
                machine.regs[source as usize] = address;
                machine.regs[index as usize] = delta;
                machine.run(&code);
                let linear = 9u32 * size as u32 + address as u32 + delta as u32;
                assert_eq!(machine.regs[Reg::Rv0 as usize], (linear % size as u32) as u16,
                    "{address}+{delta}, bank size {size}");
                assert_eq!(machine.regs[Reg::Rv1 as usize], (linear / size as u32) as u16);
            }
        }
    }
}
