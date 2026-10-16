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
