//! Behavioral GEP regressions: verify normalized addresses and banks rather
//! than requiring a particular instruction sequence or bank metadata variant.
use rcc_frontend::ir::{Value, FatPointer};
use crate::instr::{lower_gep, pointers, wide};
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::{new_function_naming, NameGenerator};
use rcc_codegen::{AsmInst, Reg};
use rcc_frontend::BankTag;
use super::machine::Machine;

pub(super) fn seed(mgr: &mut RegisterPressureManager, name: String, value: u16, insts: &mut Vec<AsmInst>) -> Reg {
    let reg = mgr.get_register(name);
    insts.extend(mgr.take_instructions());
    insts.push(AsmInst::Li(reg, value as i16));
    reg
}

pub(super) fn seed_index(mgr: &mut RegisterPressureManager, naming: &NameGenerator, id: u32, value: i32, insts: &mut Vec<AsmInst>) -> Value {
    let name = naming.temp_name(id);
    let lo = seed(mgr, name.clone(), value as u16, insts);
    mgr.pin_register(lo);
    let hi = seed(mgr, naming.i32_high_name(&name), (value >> 16) as u16, insts);
    wide::bind_i32(mgr, naming, &name, lo, hi);
    mgr.unpin_register(lo);
    Value::Temp(id)
}

pub(super) fn capture_pointer(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator, value: &Value,
    low: Reg, high: Reg, insts: &mut Vec<AsmInst>) {
    let (addr, bank) = pointers::components(mgr, naming, value, insts);
    insts.push(AsmInst::Move(low, addr));
    insts.push(AsmInst::Move(high, bank));
    mgr.unpin_register(addr);
    mgr.unpin_register(bank);
}

fn check(bank_size: u16, offset: u16, bank: BankInfo, index: i32, size: u32, dynamic: bool, pressure: bool) {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    mgr.set_memory_bank_size(bank_size);
    let mut naming = new_function_naming();
    let mut insts = vec![];
    let expected_bank = match &bank {
        BankInfo::Global => 1,
        BankInfo::Stack => 2,
        BankInfo::Heap(n) => *n,
        BankInfo::Dynamic(name) => { seed(&mut mgr, name.clone(), 10, &mut insts); 10 },
        _ => unreachable!(),
    };
    seed(&mut mgr, naming.temp_name(1), offset, &mut insts);
    mgr.set_pointer_bank(naming.temp_name(1), bank);
    let rhs = if dynamic { seed_index(&mut mgr, &naming, 2, index, &mut insts) } else { Value::Constant(index as i64) };
    if pressure {
        for i in 0..16 { seed(&mut mgr, format!("pressure_{i}"), (i * 37) as u16, &mut insts); }
    }
    insts.extend(lower_gep(&mut mgr, &mut naming, &Value::Temp(1), &[rhs], size, 10, bank_size));
    capture_pointer(&mut mgr, &mut naming, &Value::Temp(10), Reg::Rv0, Reg::Rv1, &mut insts);
    capture_pointer(&mut mgr, &mut naming, &Value::Temp(1), Reg::A0, Reg::A1, &mut insts);
    let mut machine = Machine::new(bank_size);
    machine.run(&insts);
    let linear = i64::from(expected_bank) * i64::from(bank_size) + i64::from(offset) + i64::from(index) * i64::from(size);
    assert!(linear >= 0);
    assert_eq!(machine.regs[Reg::Rv0 as usize], (linear % i64::from(bank_size)) as u16,
        "offset: bank_size={bank_size}, offset={offset}, index={index}, size={size}, dynamic={dynamic}");
    assert_eq!(machine.regs[Reg::Rv1 as usize], (linear / i64::from(bank_size)) as u16,
        "bank: bank_size={bank_size}, offset={offset}, index={index}, size={size}, dynamic={dynamic}");
    assert_eq!((machine.regs[Reg::A0 as usize], machine.regs[Reg::A1 as usize]), (offset, expected_bank), "Base pointer must remain usable");
}

#[test]
fn test_gep_simple_array_access() { check(4096, 100, BankInfo::Stack, 3, 1, false, false); }
#[test]
fn test_gep_zero_offset() { check(4096, 100, BankInfo::Global, 0, 2, false, false); }
#[test]
fn test_gep_dynamic_index() { check(4096, 100, BankInfo::Stack, 17, 2, true, false); }
#[test]
fn test_gep_power_of_two_optimization() {
    for size in [1, 2, 4, 8, 16] { check(4096, 4000, BankInfo::Heap(10), 33, size, true, false); }
}
#[test]
fn test_gep_non_power_of_two_size() { check(4096, 4000, BankInfo::Stack, 51, 3, true, false); }
#[test]
fn test_gep_large_static_offset() { check(4096, 100, BankInfo::Stack, 2000, 8, false, false); }
#[test]
fn test_gep_element_larger_than_one_word() {
    for dynamic in [false, true] {
        check(64000, 100, BankInfo::Heap(10), 2, 70000, dynamic, false);
    }
}
#[test]
fn test_gep_global_pointer() { check(4096, 1000, BankInfo::Global, 5, 2, false, false); }
#[test]
fn test_gep_fat_pointer_with_constant_addr() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    let ptr = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(4090)), bank: BankTag::Global });
    let mut insts = lower_gep(&mut mgr, &mut naming, &ptr, &[Value::Constant(10)], 4, 10, 4096);
    capture_pointer(&mut mgr, &mut naming, &Value::Temp(10), Reg::Rv0, Reg::Rv1, &mut insts);
    let mut machine = Machine::new(4096);
    machine.run(&insts);
    assert_eq!((machine.regs[Reg::Rv0 as usize], machine.regs[Reg::Rv1 as usize]), (34, 2));
}
#[test]
fn test_gep_preserves_bank_info() {
    for bank in [BankInfo::Global, BankInfo::Stack, BankInfo::Heap(10), BankInfo::Dynamic("bank".into())] {
        check(4096, 100, bank, 5, 2, false, false);
    }
}
#[test]
fn test_gep_negative_offset() {
    for dynamic in [false, true] { check(4096, 10, BankInfo::Heap(10), -5, 4, dynamic, false); }
}
#[test]
fn test_gep_register_pressure() { check(4096, 4000, BankInfo::Heap(10), 300, 7, true, true); }
#[test]
fn test_gep_runtime_bank_overflow() {
    for bank_size in [128, 4096, 64000, 65535] {
        for dynamic in [false, true] {
            for index in [0, 1, 2, 56000, 70000, -1, -100] {
                check(bank_size, bank_size - 1, BankInfo::Heap(10), index, 1, dynamic, false);
            }
        }
    }
    // Reproduces the audit failure: addition exceeds the 16-bit word width.
    check(64000, 10001, BankInfo::Heap(5), 56000, 1, true, false);
}
#[test]
fn test_gep_dynamic_with_existing_dynamic_bank() { check(4096, 4000, BankInfo::Dynamic("bank".into()), 100, 2, true, false); }
