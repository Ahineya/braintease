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

fn check(bank_size: u16, offset: u16, bank: BankInfo, index: i32, size: u32, dynamic: bool, pressure: bool) -> usize {
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
    if dynamic {
        let (lo, hi, ii) = wide::get_i32_pair(&mut mgr, &mut naming, &Value::Temp(2), false);
        insts.extend(ii);
        insts.push(AsmInst::Move(Reg::A2, lo));
        insts.push(AsmInst::Move(Reg::A3, hi));
    }
    let mut machine = Machine::new(bank_size);
    machine.run(&insts);
    let linear = (u32::from(expected_bank) * u32::from(bank_size) + u32::from(offset))
        .wrapping_add((index as u32).wrapping_mul(size));
    assert_eq!(machine.regs[Reg::Rv0 as usize], (linear % u32::from(bank_size)) as u16,
        "offset: bank_size={bank_size}, offset={offset}, index={index}, size={size}, dynamic={dynamic}");
    assert_eq!(machine.regs[Reg::Rv1 as usize], (linear / u32::from(bank_size)) as u16,
        "bank: bank_size={bank_size}, offset={offset}, index={index}, size={size}, dynamic={dynamic}");
    assert_eq!((machine.regs[Reg::A0 as usize], machine.regs[Reg::A1 as usize]), (offset, expected_bank), "Base pointer must remain usable");
    if dynamic {
        assert_eq!((machine.regs[Reg::A2 as usize], machine.regs[Reg::A3 as usize]),
                   (index as u16, (index >> 16) as u16), "Index must remain usable");
    }
    machine.executed_instructions
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

#[test]
fn test_gep_native_displacements_and_wide_fallback_match_wrapping_reference() {
    for bank_size in [128, 4096, 32768, 64000, 65535] {
        for offset in [0, bank_size / 2, bank_size - 1] {
            for index in [0, 1, 32767, 32768, 63999, 64000, 65535, 65536,
                          -1, -32768, -65535, -65536, i32::MIN, i32::MAX] {
                for size in [1, 2, 3, 16, 65536, 70000, u32::MAX] {
                    check(bank_size, offset, BankInfo::Heap(10), index, size, true, false);
                }
            }
        }
    }
}

#[test]
fn test_gep_fast_paths_preserve_pointer_tags_and_live_values_under_pressure() {
    for bank in [BankInfo::Global, BankInfo::Stack, BankInfo::Heap(10),
                 BankInfo::Dynamic("existing_bank".into())] {
        for index in [5, 56000, -5, -65535, 1234567] {
            check(64000, 63999, bank.clone(), index, 1, true, true);
        }
    }
    // A carry at 65536 must not be lost when normalizing a 64000-cell bank.
    check(64000, 63999, BankInfo::Heap(10), 65535, 1, true, true);
    // Keep the fallback's previous modulo-2^32 behavior on underflow.
    check(64000, 0, BankInfo::Global, -65535, 1, true, true);
    check(64000, 63999, BankInfo::Heap(65535), i32::MAX, 3, true, true);
    // Integer-to-pointer casts are allowed to supply a noncanonical offset.
    check(128, 65535, BankInfo::Heap(10), 1, 1, true, false);
    check(64000, 65535, BankInfo::Heap(10), -5, 1, true, false);
}

#[test]
fn test_gep_small_dynamic_indices_do_not_execute_restoring_division() {
    for (index, size) in [(0, 1), (255, 1), (63999, 1), (32767, 2), (100, 3), (-5, 4)] {
        let steps = check(64000, 63000, BankInfo::Heap(10), index, size, true, false);
        assert!(steps < 200, "bounded GEP took {steps} instructions for index {index}, size {size}");
    }
}

#[test]
fn test_gep_dynamic_provenance_tags_resolve_before_fast_arithmetic() {
    for (tag, expected_bank) in [(-1, 1), (-2, 2), (10, 10)] {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        let mut naming = new_function_naming();
        let mut insts = vec![];
        seed(&mut mgr, naming.temp_name(1), 63999, &mut insts);
        seed(&mut mgr, "tagged_bank".into(), tag as u16, &mut insts);
        mgr.set_pointer_bank(naming.temp_name(1), BankInfo::Dynamic("tagged_bank".into()));
        let index = seed_index(&mut mgr, &naming, 2, 5, &mut insts);
        insts.extend(lower_gep(&mut mgr, &mut naming, &Value::Temp(1), &[index], 1, 3, 64000));
        capture_pointer(&mut mgr, &mut naming, &Value::Temp(3), Reg::Rv0, Reg::Rv1, &mut insts);
        let mut machine = Machine::new(64000);
        machine.run(&insts);
        assert_eq!((machine.regs[Reg::Rv0 as usize], machine.regs[Reg::Rv1 as usize]), (4, expected_bank + 1));
    }
}

#[test]
fn test_gep_scaled_dynamic_offsets_match_reference_for_varied_words() {
    let mut state = 0x8d12_7a43u32;
    let mut next = || {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        state
    };
    for case in 0..1024 {
        let bank_size = [128, 4096, 32768, 64000, 65535][case % 5];
        let offset = next() as u16;
        let bank = 5 + (next() % 1000) as u16;
        let index = next() as i32;
        let size = if case % 2 == 0 { 1 + next() % 32 } else { next() };
        check(bank_size, offset, BankInfo::Heap(bank), index, size, true, case % 17 == 0);
    }
}
